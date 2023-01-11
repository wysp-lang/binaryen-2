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

#include "ir/find_all.h"
#include "ir/module-utils.h"
#include "ir/possible-constant.h"
#include "ir/subtypes.h"
#include "pass.h"
#include "support/topological_sort.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace {

/*
struct FieldInfo {
  // A map of field indexes to the globals written to them. If an index appears
  // in this map, and the value is not nullopt, then it is optimizable, which means so far we've always
  // seen the same (optimizable) global written there. If we see more than
  // one global for an index then we'll set the value to nullopt, which means
  // we've failed.
  std::unordered_map<Index, std::optional<Name>> fieldGlobals;

  // Note an optimizable global's name for an index.
  void note(Index index, Name name) {
    bool first = fieldGlobals.count(index);
    auto& fieldName = fieldGlobals[index];
    if (first) {
      // This is the first time we see this index.
      fieldName = name;
    } else if (fieldName) {
      // We've seen this index before. If the global names do not match then we
      // have failed here.
      if (*fieldName != name) {
        fieldName = std::nullopt;
      }
    } else {
      // The field contains nullopt, which means we've already failed.
    }
  }

  // Note that an index cannot be optimized.
  void noteFail(Index index) {
    fieldGlobals[index] = std::nullopt;
  }
};

using TypeFieldInfoMap = std::unordered_map<HeapType, FieldInfo>;

struct Finder : public PostWalker<Finder> {
  TypeFieldInfoMap map;

  void visitStructNew(StructNew* curr) {
    auto& fieldInfo = map[curr->type.getHeapType()];

    // Look for optimizable fields, that is, assignments of globals that contain
    // nesting.
    for (Index i = 0; i < curr->operands.size(); i++) {
      auto* operand = curr->operands[i];
      if (auto* get = operand->dynCast<GlobalGet>()) {
        auto* global = getModule()->getGlobal(get->name);
        process(global->init, fieldInfo
        if (global->init) {
          if (auto* new_ = global->init->dynCast<StructNew>()) {
            // Look one more level deep to find nesting
          }
        }
      }

      // Otherwise, we fail to optimize here.
      fieldInfo.noteFail(i);
    }
  }
};

*/

using Sequences = std::unordered_set<std::vector<Index>>;

// Given a StructNew and a prefix of the indexes we took so far to get here,
// keep looking recursively to find complete sequences.
void getSequences(StructNew* new_, std::vector<Index> prefix, Module& wasm, Sequences& out) {
  for (Index i = 0; i < curr->operands.size(); i++) {
    auto* operand = curr->operands[i];
    if (PossibleConstantValues(operand, wasm).isConstant()) {
      // This is a constant value, which is something we can optimize with,
      // so it ends a sequence. However, we can ignore it if the sequence length
      // is 1: we are looking for a nested value to cache at the outermost
      // level, so already being there means we have nothing to do.
      if (!prefix.empty()) {
        prefix.push_back(i);
        out.push_back(prefix);
        prefix.pop_back();
      }
    } else if (auto* nested = operand->dynCast<StructNew>()) {
      // This is a nested struct.new. Look deeper.
      prefix.push_back(i);
      getSequences(nested, prefix, wasm, out);
      prefix.pop_back();
    }
  }
};

struct FieldCaching : public Pass {
  // No local changes, only types and fields.
  bool requiresNonNullableLocalFixups() override { return false; }

  void run(Module* module) override {
    if (!module->features.hasGC()) {
      return;
    }

    // First, find types that we just can't optimize. The types we want to look
    // at are those that are only created in globals, so find all struct.new
    // operations in functions so that we can ignore them.
    using TypeSet = std::unordered_set<HeapType>
    ModuleUtils::ParallelFunctionAnalysis<TypeSet> analysis(
      *module, [&](Function* func, TypeSet& ignore) {
        if (func->imported()) {
          return;
        }

        for (auto* curr : FindAll<StructNew>(func->body)) {
          if (curr->type != Type::unreachable) {
            ignore.insert(curr->type.getHeapType());
          }
        }
      });

    // Merge all function info.
    TypeSet ignore;
    for (const auto& [_, funcIgnore] : analysis.map) {
      ignore.insert(funcIgnore.begin(), funcIgnore.end());
    }

    // We also want to ignore types created in a nested position in globals,
    // that is, like this:
    //
    //  (global $g
    //    (struct.new $X
    //      (struct.new $Y ..
    //
    // We can connect $X to the global directly, and optimize it, but $Y is
    // created in a nested position. We could handle it, but for simplicity for
    // now we don't.
    //
    // Also build a map of types to the globals with that type, which we'll
    // need next.
    std::unordered_map<HeapType, std::vector<Name>> typeGlobals;
    for (auto& global : module->globals) {
      if (global->imported()) {
        continue;
      }

      for (auto* curr : FindAll<StructNew>(global->init)) {
        if (curr != global->init) {
          ignore.insert(curr->type.getHeapType());
        }
      }

      if (auto* new_ = global->init->dynCast<StructNew>()) {
        auto type = new_->type.getHeapType();
        typeGlobals[type].push_back(global->name);
      }
    }

    // We found the globals with optimizable types, and can now look at them in
    // detail. Specifically we are looking for globals with nesting, like this:
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
    // To do this, we need to see the same pattern in all the globals for a
    // particular type. We'll find sequences of indexes that we can optimize,
    // such as [0,3] which means read field 0, and then read field 4. The
    // intersection of all sequences for a type is the set of things we want to
    // actually optimize. XXX move comment
    for (const auto& [type, globals] : typeGlobals) {
      if (ignore.count(type)) {
        continue;
      }

      // An entry in the map must only exist if we found a global.
      assert(!globals.empty());

      Sequences intersection;
      for (auto global : globals) {
        auto* new_ = module->getGlobal(global)->init->cast<StructNew>();
        Sequences sequences;
        getSequences(new_, {}, *module, sequences);
        if (global == globals[0]) {
          // This is the first global. Copy the sequences.
          intersection = sequences;
        } else {
          // This is a later global. Intersect with the current sequences.
          auto intersectionCopy = intersection;
          for (auto sequence : intersectionCopy) {
            if (!sequences.count(sequence)) {
              intersection.erase(sequence);
            }
          }
        }
        if (intersection.empty()) {
          // We only ever intersect here, so the set of optimizable things
          // decreases. If it ever gets to the empty set, give up.
          break;
        }
      }

      if (intersection.empty()) {
        continue;
      }

      // We found an intersection for this type! Optimize.
      // toposort
      // chak subtypes for conflicts in adding new fields
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
