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
// field to be pruned as unused. A concrete use case this can help with is to
// allow pruning repeated entries in a vtable.
//
// A particular case where this is useful is with sequences of accesses, such as
// with Java class and interface dispatch. In Java a class may implement an
// interface, which means the method appears like this:
//
//    object.itable.interface-vtable.slot_K
//
// That is, we get the itable, then get the interface vtable for a particular
// interface (say, "Hashable"), then get a particular slot in that vtable (say,
// "getHash()"). In general all such interface methods also appear in the class
// vtable as well, like this:
//
//    object.vtable.slot_N
//
// That is, we get the class vtable, then one of the slots there. We need a
// shorter sequence of operations to get to the function through the class
// vtable, so we want to do
//
//    object.itable.interface-vtable.slot_K  =>  object.vtable.slot_N
//
// This happens in practice if a particular callsite can only have a particular
// class or any of its subclasses. In that case we don't need to use the more
// generic interface dispatch method, which can handle classes with no
// connection whatsoever between them aside from them both implementing a
// particular interface. (Note that this only really helps when we have a class
// and some subclasses, and those subclasses override the vtable entry, as if
// they do not then other passes would completely devirtualize here, as they'd
// infer that only a single thing can be read from the itable.)
//
// To handle situations like this, we look not just at immediate field accesses
// like x.b but also sequences of them, x.b.g.i etc., and we look through reads
// of immutable globals while doing so (this works with Java since all the
// itables and vtables are defined in immutable globals).
//

#include "ir/module-utils.h"
#include "ir/possible-constant.h"
#include "ir/subtypes.h"
#include "ir/utils.h" // refinalize?
#include "pass.h"
#include "support/small_set.h"
#include "support/small_vector.h"
#include "support/topological_sort.h"
#include "wasm-builder.h"
#include "wasm.h"

#include "support/hash.h" // XXX
namespace std {           // XXX
// Hashing vectors is often useful
template<typename T> struct hash<vector<T>> {
  size_t operator()(const vector<T> v) const {
    auto digest = wasm::hash(v.size());
    for (const auto& t : v) {
      wasm::rehash(digest, t);
    }
    return digest;
  }
};
} // namespace std

namespace wasm {

namespace {

// A sequence of indexes that represents accesses of fields. For example, the
// sequence [5] means "read field #5", while [2, 4] means "read field #4, then
// on the object you read there, read field #2" (that is, object.field4.field2;
// note that the order has the last read in the first index as that is the
// convenient order to build up these vectors recursively from the top-most
// expression).
// Optimize this to assume a length of 3, which is the size of the itable access
// mentioned earlier.
// TODO: small 3
using Sequence = std::vector<Index>;

// Use a small set of size 1 here since the common case is to not have anything
// to optimize, that is, each value has a single sequence leading to it, which
// means just a single sequence.
// TODO: small 1
using Sequences = std::vector<Sequence>;

// Stores pairs of equivalent sequences.
struct Equivalences {
  // We store each pair once to save memory. So checking for existence etc.
  // requires two operations, see below.
  std::unordered_set<std::pair<Sequence, Sequence>> pairs;

  void add(const Sequence& a, const Sequence& b) { pairs.insert({a, b}); }

  bool has(const Sequence& a, const Sequence& b) const {
    return pairs.count({a, b}) || pairs.count({b, a});
  }

  void erase(const Sequence& a, const Sequence& b) {
    pairs.erase({a, b});
    pairs.erase({b, a});
  }

  bool empty() const { return pairs.empty(); }
};

// First, find equivalent sequences in each struct.new.
using NewEquivalencesMap = std::unordered_map<StructNew*, Equivalences>;

struct Finder : public PostWalker<Finder> {
  PassOptions& options;

  Finder(PassOptions& options) : options(options) {}

  NewEquivalencesMap map;

  // A map of values to the sequences that lead to those values. For example, if
  // we have
  //
  //    map[(ref.func $foo)] = [[0], [2, 3]]
  //
  // then that means that object.field0 == object.field2.field3 == $foo. In that
  // case we want to optimize the longer sequence to the shorter one.
  using ValueMap = std::unordered_map<PossibleConstantValues, Sequences>;

  void visitStructNew(StructNew* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }

    // Scan this struct.new, finding values and the sequences that lead to them.
    // We will recurse as we look through accesses. We start with an empty
    // sequence as our prefix, which will get built up during the recursion.
    ValueMap valueMap;
    scanNew(curr, Sequence(), valueMap);

    // Add an entry for every (reachable) struct.new. We need an entry even if
    // we find no equivalences, because that rules out optimizations later - for
    // two sequences to be equivalent, they must be equivalent in every single
    // struct.new).
    auto& entry = map[curr];

    // Fill in the entry with equivalent pairs: all sequences for the same value
    // are equivalent (the value itself no longer matters from here).
    for (auto& [_, sequences] : valueMap) {
      auto num = sequences.size();
      if (num > 1) {
        // Great, there are equivalent sequences for this value: all pairs here.
        for (size_t i = 0; i < num; i++) {
          for (size_t j = i + 1; j < num; j++) {
            entry.add(sequences[i], sequences[j]);
          }
        }
      }
    }
  }

  // Given a struct.new and a sequence prefix, look into this struct.new and add
  // anything we find into the given entry. For example, if the prefix is [1,2]
  // and we find (ref.func $foo) at index #3 then we can add a note to the entry
  // that [1,2,3] arrives at value $foo.
  void scanNew(StructNew* curr, const Sequence& prefix, ValueMap& entry) {
    // We'll only look at immutable fields.
    auto& fields = curr->type.getHeapType().getStruct().fields;

    // The current sequence will be the given prefix, plus the current index.
    auto currSequence = prefix;
    currSequence.push_back(0);

    for (Index i = 0; i < fields.size(); i++) {
      auto& field = fields[i];
      if (field.mutable_ == Mutable) {
        continue;
      }

      // Excellent, this is immutable, so we can look into the current sequence,
      // which ends with index i.
      currSequence.back() = i;

      // TODO: Look through casts, but also count them (a cast is worse than a
      // struct.get).
      // While doing so we must make sure casts succeed (or else we need to keep
      // them around in non-tnh).
      auto* operand = curr->operands[i];
      operand = Properties::getFallthrough(operand, options, *getModule());

      if (auto* subNew = operand->dynCast<StructNew>()) {
        // Look into this struct.new recursively.
        scanNew(subNew, currSequence, entry);
        continue;
      }

      // See if this is a constant value.
      PossibleConstantValues value;
      value.note(operand, *getModule());
      if (value.isConstantLiteral() || value.isConstantGlobal()) {
        // Great, this is something we can track.
        entry[value].push_back(currSequence);

        if (value.isConstantGlobal()) {
          // Not only can we track the global itself, but we may be able to look
          // into the object created in the global.
          auto* global = getModule()->getGlobal(value.getConstantGlobal());
          if (!global->imported()) {
            if (auto* subNew = global->init->dynCast<StructNew>()) {
              scanNew(subNew, currSequence, entry);
            }
          }
        }
      }
    }
    // TODO Handle more cases like a tee and a get (with nothing in the middle).
    //      See related code in OptimizeInstructions that can perhaps be
    //      shared.
  }
};

using TypeEquivalencesMap = std::unordered_map<HeapType, Equivalences>;

struct EquivalentFieldOptimization : public Pass {
  // Only modifies types.
  bool requiresNonNullableLocalFixups() override { return false; }

  void run(Module* module) override {
    if (!module->features.hasGC()) {
      return;
    }

    // First, find all the relevant sequences inside each function.
    ModuleUtils::ParallelFunctionAnalysis<NewEquivalencesMap> analysis(
      *module, [&](Function* func, NewEquivalencesMap& map) {
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

    // Combine all the info. The property we seek is which sequences are
    // equivalent, that is, lead to the same value. For two sequences to be
    // equal they must be equal in every single struct.new for that type (and
    // subtypes, see below).
    TypeEquivalencesMap unifiedMap;

    // Given a type and equivalences we found for it somewhere, merge that into
    // the main unified map.
    auto mergeIntoUnifiedMap = [&](HeapType type,
                                   const Equivalences& currEquivalences) {
      auto iter = unifiedMap.find(type);
      if (iter == unifiedMap.end()) {
        // This is the first time we see this type. Just copy the data, there is
        // nothing to compare it to yet.
        unifiedMap[type] = currEquivalences;
      } else {
        // This is not the first time, so we must filter what we've seen so far
        // with the current data: anything we thought was equivalent before, but
        // is not so in the new data, is not globally equivalent.
        //
        // Iterate on a copy to avoid invalidation. TODO optimize
        auto& unifiedEquivalences = iter->second;
        auto copy = unifiedEquivalences;
        for (auto& pair : copy.pairs) {
          if (!currEquivalences.has(pair.first, pair.second)) {
            unifiedEquivalences.erase(pair.first, pair.second);
          }
        }
      }
    };

    for (const auto& [_, map] : analysis.map) {
      for (const auto& [curr, equivalences] : map) {
        mergeIntoUnifiedMap(curr->type.getHeapType(), equivalences);
      }
    }
    for (const auto& [curr, equivalences] : moduleFinder.map) {
      mergeIntoUnifiedMap(curr->type.getHeapType(), equivalences);
    }

    // Check if we found anything to work with.
    auto foundWork = [&]() {
      for (auto& [type, equivalences] : unifiedMap) {
        if (!equivalences.empty()) {
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
      // super either. This is basically more information to merge into the
      // unified map, like before: as we merge information in, we filter to
      // leave the intersection of all sequences.
      for (auto subType : subTypeAnalyzer.subTypes.getStrictSubTypes(type)) {
        mergeIntoUnifiedMap(type, unifiedMap[subType]);
      }
    }

    // We may have filtered out all the possible work, so check again.
    if (!foundWork()) {
      return;
    }

    // Excellent, we have things we can optimize with!
    FunctionOptimizer(unifiedMap).run(getPassRunner(), module);
  }

  struct FunctionOptimizer : public WalkerPass<PostWalker<FunctionOptimizer>> {
    bool isFunctionParallel() override { return true; }

    // Only modifies struct.get operations.
    bool requiresNonNullableLocalFixups() override { return false; }

    std::unique_ptr<Pass> create() override {
      return std::make_unique<FunctionOptimizer>(unifiedMap);
    }

    FunctionOptimizer(TypeEquivalencesMap& unifiedMap)
      : unifiedMap(unifiedMap) {}

    void visitStructGet(StructGet* curr) {
      if (curr->type == Type::unreachable) {
        return;
      }

      auto iter = unifiedMap.find(curr->type.getHeapType());
      if (iter == unifiedMap.end()) {
        return;
      }

      // This heap type has information about possible sequences to optimize.
      auto& equivalences = iter->second;

      // TODO: long sequences, i.e., of more then 1.
      Sequence currSequence = {curr->index};

      // TODO: Make this more efficient than a brute-force search through all
      //       pairs. However, the number of pairs is usually short so this
      //       might be ok for now.

      // Look for a better sequence: either shorter, or using lower indexes.
      Sequence best = currSequence;
      auto maybeUse = [&](const Sequence& s) {
        // TODO: full lexical < on 1,2,3,.. etc. once we support long
        //       sequences
        if (s.size() < best.size() || s[0] < best[0]) {
          best = s;
        }
      };

      for (auto& [a, b] : equivalences.pairs) {
        if (a == currSequence) {
          maybeUse(b);
        } else if (b == currSequence) {
          maybeUse(a);
        }
      }

      curr->index = best[0];
    }

    void doWalkFunction(Function* func) {
      WalkerPass<PostWalker<FunctionOptimizer>>::doWalkFunction(func);

      // If we changed anything, we need to update parent types as types may
      // have changed. XXX is this needed?
      if (changed) {
        ReFinalize().walkFunctionInModule(func, getModule());
      }
    }

  private:
    TypeEquivalencesMap& unifiedMap;

    bool changed = false; // XXX
  };
};

} // anonymous namespace

Pass* createEquivalentFieldOptimizationPass() {
  return new EquivalentFieldOptimization();
}

} // namespace wasm
