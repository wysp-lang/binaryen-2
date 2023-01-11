/*
 * Copyright 2023 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Cache struct fields when that seems helpful. For example, imagine that we
// have a struct that is often accessed using ref.foo.bar. If that access
// happens very often, perhaps we should cache it on the object itself, allowing
// us to replace ref.foo.bar with ref.foo_cached_bar - a single load instead of
// two. This has downsides, of course, in increasing the size of the object, and
// so in general it is risky to do, but at least in one type of situation it can
// almost always be expected to help:
//
//  * The object we are adding the cached field onto is only created a fixed
//    number of times.
//
// In particular, if the object is only created in globals then we can see all
// those instances at compile time. And in that case, the extra memory usage is
// definitely bounded, while the speedup of having fewer operations can end up
// substantial, so it is worth caching.
//
// We do not do a very sophisticated analysis here. Aside from the above, we
// also check for properties like:
//
//  * An immutable field.
//  * The field contains something we can copy, like a function reference.
//
// (without those, it is very unlikely anything useful can be done). But aside
// from those we will cache all the things that look relevant. Later passes can
// then use the cached field. If they do not, then GlobalTypeOptimization will
// prune those fields, so running this pass should have no downside (aside from
// compile time).
//

#include <algorithm>

#include "ir/module-utils.h"
#include "ir/possible-constant.h"
#include "ir/subtypes.h"
#include "pass.h"
#include "support/small_set.h"
#include "support/small_vector.h"
#include "support/topological_sort.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace {

struct FieldCaching : public Pass {
  // No local changes, only types and fields.
  bool requiresNonNullableLocalFixups() override { return false; }

  void run(Module* module) override {
    if (!module->features.hasGC()) {
      return;
    }

    // First, find all the relevant improvements inside each function. We are
    // looking for heap types that have an optimization opportunity, which looks
    // like this:
    //
    //   x = new Struct(.., g, ..);
    //
    // where g is a global with nesting, like this:
    //
    //  global g = {
    //    ..
    //    {
    //      value
    //    },
    //    ..
    //  };
    //
    // |value| is nested, and we'd like to cache it at a higher level, like
    // this:
    //
    //  global g = {
    //    ..
    //    {
    //      value
    //    },
    //    ..,
    //    value
    //  };
    //
    // After that, we can replace x.foo.bar with x.newfield, using the new field
    // we just added at the end.
    //
    // To optimize, we need all creations of a particular struct type to have
    // the same optimization pattern
    ModuleUtils::ParallelFunctionAnalysis<NewImprovementsMap> analysis(
      *module, [&](Function* func, NewImprovementsMap& map) {
        if (func->imported()) {
          return;
        }

        Finder finder(getPassOptions());
        finder.walkFunctionInModule(func, module);
        map = std::move(finder.map);
      });

    // Also look in the module scope.
    Finder moduleFinder(getPassOptions());
    moduleFinder.walkModuleCode(module);

    // Combine all the info. The property we seek is an improvement which is
    // valid in all the struct.news of a particular type. When that is the case
    // we can apply the improvement in any position in the module. To find that,
    // we'll merge information into a unified map.
    TypeImprovementMap unifiedMap;

    // Given a type and some improvements we found for it somewhere, merge that into
    // the main unified map.
    auto mergeIntoUnifiedMap = [&](HeapType type,
                                   const Improvements& currImprovements) {
      auto iter = unifiedMap.find(type);
      if (iter == unifiedMap.end()) {
        // This is the first time we see this type. Just copy the data, there is
        // nothing to compare it to yet.
        unifiedMap[type] = currImprovements;
        return;
      }

      // This is not the first time, so we must filter what we've seen so far
      // with the current data: anything we've seen so far as consistently
      // improvable must also be improvable in this new info, or else it must
      // not be optimized. That means we want to take the intersection of
      // the sets of improved sequences, for each sequence.
      auto& typeImprovements = iter->second;
                              
      for (auto& [sequence, typeImprovementSet] : typeImprovements) {
        auto iter = currImprovements.find(sequence);
        if (iter == currImprovements.end()) {
          // Nothing at all, so the intersection is empty.
          typeImprovementSet.clear();
          continue;
        }

        auto& currImprovementSet = iter->second;
        ImprovementSet intersection;
        for (auto& s : currImprovementSet) {
          if (typeImprovementSet.count(s)) {
            intersection.insert(s);
          }
        }
        typeImprovementSet = intersection;
      }
    };

    for (const auto& [_, map] : analysis.map) {
      for (const auto& [curr, improvements] : map) {
        mergeIntoUnifiedMap(curr->type.getHeapType(), improvements);
      }
    }
    for (const auto& [curr, improvements] : moduleFinder.map) {
      mergeIntoUnifiedMap(curr->type.getHeapType(), improvements);
    }

    // We have found all the improvement opportunities in the entire module.
    // Stop if there are none.
    auto foundWork = [&]() {
      for (auto& [type, improvements] : unifiedMap) {
        if (!improvements.empty()) {
          return true;
        }
      }
      return false;
    };
    if (!foundWork()) {
//std::cerr << "nada\n";
      return;
    }

    // Apply subtyping: To consider fields i, j equivalent in a type, we also
    // need them to be equivalent in all subtypes.
    struct SubTypeAnalyzer : public TopologicalSort<HeapType, SubTypeAnalyzer> {
      SubTypes subTypes;

      SubTypeAnalyzer(Module& module) : subTypes(module) {
        // The roots are types with no super.
        for (auto type : subTypes.types) {
          auto super = type.getSuperType();
          if (!super) {
            push(type);
          }
        }
      }

      void pushPredecessors(HeapType type) {
        // We must visit subtypes before ourselves.
        for (auto subType : subTypes.getStrictSubTypes(type)) {
          push(subType);
        }
      }
    };

    SubTypeAnalyzer subTypeAnalyzer(*module);
    for (auto type : subTypeAnalyzer) {
      // We have visited all subtypes, and can use their information here,
      // namely that if a pair is not equivalent in a subtype, it isn't in the
      // super either. This is basically more information to merge into the
      // unified map, like before: as we merge information in, we filter to
      // leave the intersection of all sequences.
      for (auto subType : subTypeAnalyzer.subTypes.getStrictSubTypes(type)) {
        mergeIntoUnifiedMap(type, unifiedMap[subType]);
      }
    }

    // We may have just filtered out all the possible work, so check again.
    if (!foundWork()) {
//std::cerr << "nada2\n";
      return;
    }

    // Excellent, we have things we can optimize with!
    //
    // Before optimizing, prune the data. We have a set of possible improvements
    // for each sequence, but after merging everything as we have, we just care
    // about the best one for each sequence (as that is what we'll pick whenever
    // we find a place to optimize.
    for (auto& [_, improvements] : unifiedMap) {
      for (auto& [_, newSequences] : improvements) {
        if (newSequences.size() > 1) {
          std::optional<Sequence> best;
          for (auto& s : newSequences) {
            if (!best || s.size() < best->size() || (s.size() == best->size() && s < *best)) {
              best = s;
            }
          }
          assert(best);
          newSequences = {*best};
        }
      }
    }

    // Optimize.
    FunctionOptimizer(unifiedMap).run(getPassRunner(), module);
  }

  struct FunctionOptimizer : public WalkerPass<PostWalker<FunctionOptimizer>> {
    bool isFunctionParallel() override { return true; }

    // Only modifies struct.get operations.
    bool requiresNonNullableLocalFixups() override { return false; }

    std::unique_ptr<Pass> create() override {
      return std::make_unique<FunctionOptimizer>(unifiedMap);
    }

    FunctionOptimizer(TypeImprovementMap& unifiedMap)
      : unifiedMap(unifiedMap) {}

    void visitStructGet(StructGet* curr) {
      optimizeSequence(curr);
    }

    void visitRefCast(RefCast* curr) {
      optimizeSequence(curr);
    }

    void optimizeSequence(Expression* curr) {
      if (curr->type == Type::unreachable) {
        return;
      }

      // The current sequence of operations. We'll go deeper and build up the
      // sequence as we go, looking for improvements as we go.
      //
      // TODO: use a fallthrough here. a Tee in the middle should not stop us.
      Sequence currSequence;

      // The start of the sequence - the reference that the sequence of field
      // accesses begins with.
      Expression* currStart = curr;
//std::cerr << "\noptimizeSequence in visit: " << *curr << '\n';
      while (1) {

//std::cerr << "loop inspect sequence for " << *currStart << "\n";

        // Apply the current value to the sequence, and point currStart to the
        // item we are reading from right now (which will be the next item
        // later, and is also the reference from which the entire sequence
        // begins).
        if (auto* get = currStart->dynCast<StructGet>()) {
          currStart = get->ref;
          currSequence.push_back(Item(get->index));
        } else if (auto* cast = currStart->dynCast<RefCast>()) {
          currStart = cast->ref;
          // Nothing to add to the sequence, as it contains only field lookups.
          // If we actually optimize, it will be to a sequence with no casts, so
          // the cast will end up optimized out anyhow.
        } else {
          // The sequence ended.
          break;
        }

//for (auto x : currSequence) std::cerr << x << ' ';
//std::cerr << '\n';

        // See if a sequence starting here has anything we can optimize with.
        // TODO: we could also look at our supertypes
        auto iter = unifiedMap.find(currStart->type.getHeapType());
        if (iter == unifiedMap.end()) {
//std::cerr << "  sad1\n";
          continue;
        }

        auto& improvements = iter->second;
        auto iter2 = improvements.find(currSequence);
        if (iter2 != improvements.end()) {
          // Wonderful, we can optimize here! Replace the current sequence of
          // operations with the new ones of the new and better sequence.
          // TODO Rather than use this immediately, we could consider longer
          //      sequences first, and pick the best.
          auto& newSequences = iter2->second;
          if (!newSequences.empty()) {
            assert(newSequences.size() == 1);
            auto& newSequence = *newSequences.begin();
            replaceCurrent(buildSequence(newSequence, currStart));
            return;
          }
        }
//std::cerr << "  sad2\n";
      }
    }

    // Given a sequence of field accesses, and a starting reference from which
    // to begin applying them, build struct.gets for that sequence and return
    // them.
    Expression* buildSequence(const Sequence& s, Expression* start) {
      auto* result = start;

      // Starting from the top, build up the new stack of instructions.
      for (Index i = 0; i < s.size(); i++) {
        auto index = s[s.size() - i - 1];
        auto fields = result->type.getHeapType().getStruct().fields;
        // We must be able to read the field here. A possible bug here is if a
        // cast is needed, but we are only aiming to optimize to sequences with
        // no casts, and we should have ignored casting sequences before.
        assert(index < fields.size());
        auto type = fields[index].type;
        result = Builder(*getModule()).makeStructGet(index, result, type);
      }

      return result;
    }

  private:
    TypeImprovementMap& unifiedMap;
  };
};

} // anonymous namespace

Pass* createFieldCachingPass() {
  return new FieldCaching();
}

} // namespace wasm
