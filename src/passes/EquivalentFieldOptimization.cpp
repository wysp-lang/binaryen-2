/*
 * Copyright 2022 WebAssembly Community Group participants
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
// Find struct fields that are always equal, and convert all reads to the first
// of them. For example:
//
//   x = new Foo(a: 5, b: 5);
//   y = new Foo(a: 7, b: 7);
//
// The fields a and b are always equal, so we can read either one to get the
// value, which means we can do this:
//
//   x.b  =>  x.a
//   y.b  =>  y.a
//
// By always reading from the earlier field we increase the chance for the later
// field to be pruned as unused.
//

#include "ir/module-utils.h"
#include "ir/possible-constant.h"
#include "ir/subtypes.h"
#include "pass.h"
#include "support/topological_sort.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

// To find things we can optimize, we focus on pairs of immutable fields that
// always - in all struct.news - begin identical. We find all such pairs in each
// struct.new by scanning all the code, then we'll merge that together and
// optimize using that information.

struct Pair {
  // A pair of indexes (of fields), canonicalized to be in order.
  Index low;
  Index high;
  Pair(Index low, Index high) : low(low), high(high) {
    assert(low <= high);
  }
};

using Pairs = std::unordered_set<Pair>;

using PairsMap = std::unordered_map<StructNew*, Pairs>;

struct FieldFinder : public PostWalker<FieldFinder> {
  PassOptions& options;

  FieldFinder(
  PassOptions& options) : options(options) {}

  PairsMap map;

  void visitStructNew(StructNew* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }

    // Add an entry for every (reachable) struct.new. We need an entry even if
    // there are no equivalent pairs, because that rules out the type having any
    // such pairs globally (a pair must be equivalent in every single new).
    auto& entry = map[curr->type.getHeapType()];

    // Find pairs of immutable fields with equal values.
    auto& fields = curr->type.getHeapType().getStruct().fields;
    for (Index i = 0; i < fields.size(); i++) {
      auto& iField = fields[i];
      if (iField.mutability == Mutable) {
        continue;
      }
      for (Index j = i + 1; j < fields.size(); j++) {
        auto& jField = fields[j];
        if (jField.mutability == Mutable) {
          continue;
        }

        // Great, fields i and j are both immutable.

        // See if they have the same declaration (type and packing).
        if (iField != jField) {
          continue;
        }

        // Finally, see if their values match.
        if (curr->isWithDefault() || areEqual(curr->operands, i, j)) {
          entry.insert(Pair(i, j));
        }
      }
    }
  }

  bool areEqual(const ExpressionList& list, Index i, Index j) {
    // TODO Handle more cases like a tee and a get (with nothing in the middle).
    //      See related code in OptimizeInstructions that can perhaps be
    //      shared. For now just handle immutable globals and constants.
    // TODO Fallthrough.
    PossibleConstantValues iValue;
    iValue.note(list[i], *getModule());
    if (!iValue.isConstantLiteral() && !iValue.isConstantGlobal()) {
      return false;
    }
    PossibleConstantValues jValue;
    iValue.note(list[j], *getModule());
    return iValue == jValue;
  }
};

// Given a set, and another set to test against, remove all items in the first
// set that are not in the second. That is,
//
//  set => set - test
//
template<typename T>
void eraseItemsNotIn(T& set, const T& test) {
  std::vector<T> toDelete;
  for (auto x : set) {
    if (test.count(x) == 0) {
      toDelete.push_back(x);
    }
  }
  for (auto x : toDelete) {
    set.erase(x);
  }
}

struct EquivalentFieldOptimization : public Pass {
  // Only modifies types.
  bool requiresNonNullableLocalFixups() override { return false; }

  Module* module;

  // The types we can merge. We map each such type to merge with the type we
  // want to merge it with.
  using TypeUpdates = std::unordered_map<HeapType, HeapType>;
  TypeUpdates merges;

  void run(Module* module_) override {
    module = module_;

    if (!module->features.hasGC()) {
      return;
    }

    // First, find all the equivalent pairs.

    ModuleUtils::ParallelFunctionAnalysis<PairsMap> analysis(
      *module, [&](Function* func, PairsMap& map) {
        if (func->imported()) {
          return;
        }

        FieldFinder finder(getPassOptions());
        finder.walkFunctionInModule(func, *module);
        map = std::move(finder.map);
      });

    // Also find struct.news in the module scope.
    FieldFinder moduleFinder(getPassOptions());
    moduleFinder.walkModuleCode(module);

    // Combine all the maps of equivalent indexes. For a pair of indexes to be
    // truly equivalent globally they must be equivalent in every single
    // struct.new of that type.
    std::unordered_map<HeapType, Pairs> unifiedMap;

    auto processStructNew = [&](StructNew* curr, const Pairs& pairs) {
      auto type = structNew->type.getHeapType();
      // This is the first time we see this type if we insert a new entry now.
      auto [iter, first] = unifiedMap.insert(type, {});
      auto& typePairs = iter->second;
      if (first) {
        // Just copy all the pairs we've seen.
        typePairs = pairs;
      } else {
        // This is not the first time, so the current equivalent fields are a
        // filter: anything we thought was equivalent before, but is not
        // present now, is not globally equivalent.
        eraseItemsNotIn(typePairs, pairs);
      }
    };

    for (const auto& [_, map] : analysis.map) {
      for (const auto& [curr, pairs] : map) {
        processStructNew(curr, pairs);
      }
    }
    for (const auto& [curr, pairs] : moduleFinder.map) {
      processStructNew(curr, pairs);
    }

    // Check if we found anything to work with.
    auto foundWork = [&]() {
      for (auto& [type, pairs] : unifiedMap) {
        if (!pairs.empty()) {
          return true;
        }
      }
      return false;
    };
    if (!foundWork()) {
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
      // super either.
      for (auto subType : subTypes.getStrictSubTypes(type)) {
        eraseItemsNotIn(unifiedMap[type], unifiedMap[subType]);
      }
    }

    // We may have filtered out all the possible work, so check again.
    if (!foundWork()) {
      return;
    }

    // Excellent, we have things we can optimize with!
  }
};

} // anonymous namespace

Pass* createEquivalentFieldOptimizationPass() { return new EquivalentFieldOptimization(); }

} // namespace wasm
