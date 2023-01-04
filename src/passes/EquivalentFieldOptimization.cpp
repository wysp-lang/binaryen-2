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
// field to be pruned as unused. An example use case this can help with is to
// allow pruning repeated entries in a vtable.
//
// A particular case where this is useful is with sequences of accesses, such as
// with Java class and interface dispatch. In J2Wasm a class may implement an
// interface, which means the method appears like this:
//
//    object.itable.interface_vtable_M.slot_K
//
// That is, we get the itable, then get the interface vtable for a particular
// interface (say, "Hashable"), then get a particular slot in that vtable (say,
// "getHash()"). In general all such interface methods also appear in the class
// vtable as well, like this:
//
//    object.vtable.slot_R
//
// That is, we get the class vtable, then one of the slots there. We need a
// shorter sequence of operations to get to the function through the class
// vtable, so we want to do this optimization:
//
//    object.itable.interface_vtable_M.slot_K  =>  object.vtable.slot_R
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
// We can also remove casts in some cases. The interface pattern above in fact
// requires a cast in J2Wasm when we go from the very general itable, which has
// generic fields, to a specific interface vtable. Class-based dispatch is both
// shorter, and avoids that cast.
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

// We will be comparing sequences of accesses. For example,
//
//    object.itable.interface_vtable_M.slot_K
//
// has three accesess, first we load the itable, then one of its fields, then
// one of the fields of that interface vtable. For simplicity we denote each
// access by the index of the field that is loaded from the relevant struct. (We
// can infer the type as we go, so there is no need to store that as well.) Thus
// the items in our sequences are simply indexes.
using Item = Index;
using Sequence = std::vector<Item>;
using Sequences = std::vector<Sequence>;

// An improvement is a sequence that we want to turn into another sequence,
// because the latter sequence is better.
using Improvement = std::pair<Sequence, Sequence>;
using Improvements = std::unordered_set<Improvement>;

// That is, if we reach a point that we load
// something with a type that is not refined enough for us to perform the load
// after it (say, if a field were anyref then that would be the case). We don't
// need to track more specifically than that, as we'll follow the simple rule of
// never optimizing anything to a sequence that requires a cast - we only want
// to generate new code with no casts, either if it had no casts before, or if
// we can remove a cast. This is sufficient at least for J2Wasm
// TODO Consider other situations with multiple casts and where it is worthwhile
//      to leave a cast or even add one.
// TODO: small 3

// In our first phase we will find equivalent sequences in each struct.new in
// the entire program, and then which are useful improvements. For simplicity
// we'll gather them in a map whose key is the struct.new they derive from, in
// the Finder class. Later we'll merge all that together: an improvement is
// valid if it is present in all struct.news in the entire program.

using NewImprovementsMap = std::unordered_map<StructNew*, Improvements>;

struct Finder : public PostWalker<Finder> {
  PassOptions& options;

  Finder(PassOptions& options) : options(options) {}

  NewImprovementsMap map;

  // A map of values to the sequences that lead to those values. For example, if
  // we have
  //
  //    map[(ref.func $foo)] = [[0], [2, 3]]
  //
  // then that means that object.field0 == object.field2.field3 == $foo. In that
  // case we want to optimize the longer sequence to the shorter one.
  //
  // Going back to the example in the top of this file, we would find that the
  // first struct.new has [0] and [1] for value 5, and the second struct.new has
  // the same but for value 7. [0] and [1] are equivalent in both cases, which
  // will allow optimization later, when we just remember the equivalences and
  // not the particular values in each struct.new.
  //
  // Note that we use a PossibleConstantValues here as we want to handle not
  // just Literals but also GlobalGets of immutable things, which is how itables
  // and vtables are declared.
  using ValueMap = std::unordered_map<PossibleConstantValues, Sequences>;

  void visitStructNew(StructNew* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }

    // All sequences will begin with the type we are creating right here.
    auto startType = curr->type.getHeapType();

    // Scan this struct.new, finding values and the sequences that lead to them.
    // We will recurse as we look through accesses. We start with an empty
    // sequence as our prefix, which will get built up during the recursion.
    ValueMap valueMap;
    scanNew(curr, Sequence(), valueMap);

    // We now have a map of values to the sequences that get to them, which
    // means we know which sequences are equivalent in this struct.new, and can
    // start to build an entry for it in the global map. Note we need an entry
    // even if we find no equivalences, because that means there are no
    // equivalences at all (which will prevent optimizations later).
    auto& improvements = map[curr];
//std::cerr << "apply in " << getModule()->typeNames[curr->type.getHeapType()].name << '\n';

    for (auto& [value, sequences] : valueMap) {
      // The final type each sequence arrives at is the known value.
      auto finalType = value.getType(*getModule());

      auto num = sequences.size();
      if (num > 1) {
        // Great, there are equivalent sequences for this value. All the pairs
        // here are potential improvements. Find the actual ones and add them.
        for (size_t i = 0; i < num; i++) {
          for (size_t j = i + 1; j < num; j++) {
            // Given a pair (A, B), A may be an improvement on B, or B on A, but
            // never both ways.
            // TODO cache the cast checks and other operations here
            addIfImprovement(sequences[i], sequences[j], improvements, startType, finalType) ||
              addIfImprovement(sequences[j], sequences[i], improvements, startType, finalType);
          }
        }
      }
    }
  }

  // Add (A, B) (an improvement from A to B) if it is indeed an improvement.
  // Return true if so.
  bool addIfImprovement(const Sequence& a, const Sequence& b, Improvements& improvements, HeapType startType, Type finalType) {
    auto aSize = a.size();
    auto bSize = b.size();
    if (bSize > aSize) {
      // B is larger, so give up.
      // TODO Perhaps if B has no casts but A does, it is worth it?
      return false;
    }

    // B is smaller or of equal size. Casts will determine what we do here.

    if (requiresCast(b, startType, finalType)) {
      // We never optimize to something with a cast.
      // TODO: Perhaps if both have casts, we should still optimize?
      return false;
    }

    if (requiresCast(a, startType, finalType) || bSize < aSize) {
      // We are either getting rid of a cast, or neither have casts but B is
      // shorter, so it is a valid improvement.
      //
      // We insert the reversed sequence, as that is how we will be using it
      // later TODO explain with example
      auto reverseA = a;
      auto reverseB = b;
      std::reverse(reverseA.begin(), reverseA.end());
      std::reverse(reverseB.begin(), reverseB.end());
//std::cerr << "add sequence for " << value << " : ";
//for (auto x : reverse) std::cerr << x << ' ';
//std::cerr << '\n';

      improvements.insert({reverseA, reverseB});
      return true;
    }

    return false;
  }

  // Given a sequence of field lookups starting from a particular type, see if
  // we require a cast to perform its field lookups lookings, when going from
  // the start type all the way to the final type we expect at the end.
  bool requiresCast(const Sequence& s, HeapType startType, Type finalType) {
    // Track the current type as we go. Nullability does not matter here, but we
    // do need to handle the case of a non-heap type, as the final type may be
    // such.
    auto type = Type(startType, Nullable);
    for (auto i : s) { // XXX reversed later down, so reverse iteration here?
      // Check if we can do the current lookup using the current type.
      auto& fields = type.getHeapType().getStruct().fields;
      if (i >= fields.size()) {
        // This field does not exist in this type - it is added in a subtype. So
        // a cast is necessary.
        return true;
      }

      // Continue onwards.
      type = fields[i].type;
    }

    // We must have arrived at the proper final type, or else we need a cast
    // there.
    return type != finalType;
  }

  // Given a struct.new and a sequence prefix, look into this struct.new and add
  // anything we find into the given valueMap. For example, if the prefix is [1,2]
  // and we find (ref.func $foo) at index #3 then we can add a note to the valueMap
  // that [1,2,3] arrives at value $foo.
  //
  // We also receive the "storage type" - the type of the location this data is
  // stored in. If it is stored in a less-refined location then we will need a
  // cast to read from it.
  void scanNew(StructNew* curr, const Sequence& prefix, ValueMap& valueMap) {
    // We'll only look at immutable fields.
    auto& fields = curr->type.getHeapType().getStruct().fields;

    // The current sequence will be the given prefix, plus a possible cast, then
    // plus the current index. Add a 0 for the current index, which we will then
    // increment as we go.
    auto currSequence = prefix;
    currSequence.push_back(Item(0));

    for (Index i = 0; i < fields.size(); i++) {
      auto& field = fields[i];
      if (field.mutable_ == Mutable) {
        continue;
      }

      // TODO: disallow packed fields for now, we need to encode that.

      // This is a field we can work with, so we can keep going, with this index
      // added.
      currSequence.back() = i;

      processChild(curr->operands[i], currSequence, valueMap);
    }
  }

  // Note that unlike scanStructNew, this is given the current
  // sequence, which also encodes the current expression (the other two are
  // given a prefix that they append to).
  void processChild(Expression* curr, const Sequence& currSequence, ValueMap& valueMap) {
    if (auto* subNew = curr->dynCast<StructNew>()) {
      // Look into this struct.new recursively.
      scanNew(subNew, currSequence, valueMap);
      return;
    }

    // See if this is a constant value.
    PossibleConstantValues value;
    value.note(curr, *getModule());
    if (value.isConstantLiteral() || value.isConstantGlobal()) {
      // Great, this is something we can track.

      valueMap[value].push_back(currSequence);

      if (value.isConstantGlobal()) {
        // Not only can we track the global itself, but we may be able to look
        // into the object created in the global.
        auto* global = getModule()->getGlobal(value.getConstantGlobal());
        // We already checked the global is immutable via isConstantGlobal.
        assert(!global->mutable_);
        if (!global->imported()) {
          if (auto* subNew = global->init->dynCast<StructNew>()) {
            scanNew(subNew, currSequence, valueMap);
          }
        }
      }
    }

    // TODO Handle more cases like a tee and a get (with nothing in the middle).
    //      See related code in OptimizeInstructions that can perhaps be
    //      shared.
  }
};

// As we optimize, we will find the current sequence used in a particular spot,
// and will check in a map of possible improvements whether that sequence has an
// improvement in fact.
using ImprovementMap =  std::unordered_map<Sequence, Sequence>;
using TypeImprovementMap = std::unordered_map<HeapType, ImprovementMap>;

struct EquivalentFieldOptimization : public Pass {
  // Only modifies types.
  bool requiresNonNullableLocalFixups() override { return false; }

  void run(Module* module) override {
    if (!module->features.hasGC()) {
      return;
    }

    // First, find all the relevant improvements inside each function.
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
                                   const ImprovementMap& currImprovements) {
      auto iter = unifiedMap.find(type);
      if (iter == unifiedMap.end()) {
        // This is the first time we see this type. Just copy the data, there is
        // nothing to compare it to yet.
        unifiedMap[type] = currImprovements;
      } else {
        // This is not the first time, so we must filter what we've seen so far
        // with the current data: anything we've seen so far as consistently
        // improvable must also be improvable in this new info, or else it must
        // not be optimized.

        // Iterate on a copy to avoid invalidation. TODO optimize
        auto& typeImprovements = iter->second;
        auto copy = typeImprovements;
        for (auto& improvement : typeImprovements) {
          auto iter = currImprovements.find(improvement.first);
          if (iter == currImprovements.end() ||
              iter->second != improvement.second) {
            // Either this existing improvement is not present in the current
            // set, or there is something else there, so we do not see the
            // consistency we are looking for.
            // TODO: Perhaps if there is something different, we can find the
            //       intersection.
            typeImprovements.erase(iter);
          }
        }
      }
    };

    auto mergeMapIntoUnifiedMap = [&](HeapType type,
                                   const Improvements& currImprovements) {
      ImprovementMap currImprovementMap;
      for (const auto& improvement : currImprovements) {
        currImprovementMap[improvement.first] = improvement.second;
      }
      mergeIntoUnifiedMap(type, currImprovementMap);
    };

    for (const auto& [_, map] : analysis.map) {
      for (const auto& [curr, improvements] : map) {
        mergeMapIntoUnifiedMap(curr->type.getHeapType(), improvements);
      }
    }
    for (const auto& [curr, improvements] : moduleFinder.map) {
      mergeMapIntoUnifiedMap(curr->type.getHeapType(), improvements);
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

      Sequence currSequence;
      Expression* currValue = curr;
//std::cerr << "\noptimizeSequence in visit: " << *curr << '\n';
      while (1) {

//std::cerr << "inspect sequence for " << *currValue << "\n";

        // Apply the current value to the sequence, and point currValue to the
        // item we are reading from right now (which will be the next item
        // later, and is also the reference from which the entire sequence
        // begins).
        if (auto* get = currValue->dynCast<StructGet>()) {
          currValue = get->ref;
          currSequence.push_back(Item(get->index));
        } else if (auto* cast = currValue->dynCast<RefCast>()) {
          currValue = cast->ref;
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
        auto iter = unifiedMap.find(currValue->type.getHeapType());
        if (iter == unifiedMap.end()) {
          continue;
        }
        auto& improvements = iter->second;
//std::cerr << "seek in " << getModule()->typeNames[currValue->type.getHeapType()].name << '\n';

        // Look at everything equivalent to this sequence.
        //
        // TODO: Make this more efficient than a brute-force search to find the
        //       improvements and then on those improvements. However, there are
        //       usually not that many improvements anyhow.

        // If we find a sequence better than currSequence, we'll set it here.
        std::optional<Sequence> best;

        auto maybeUse = [&](const Sequence& s) {
//std::cerr << "mayyyyyyyyybe\n";
//for (auto x : s) std::cerr << x << ' ';
//std::cerr << '\n';

//std::cerr << "waka " << s.size() << " : " << best->size() << " : " << currSequence.size() << " : " << (s < *best) << '\n';//) {
          if (isBetter(s, currSequence)) {
            if (!best || isBetter(s, *best)) {
              best = s;
            }
          }
        };

        for (auto& [a, b] : improvements.pairs) {
          if (a == currSequence) {
            maybeUse(b);
          } else if (b == currSequence) {
            maybeUse(a);
          }
        }

        if (best) {
//std::cerr << "  yes\n";
          // We found a better sequence! Apply it, going step by step through
          // the sequence.

          Builder builder(*getModule());

          // |currValue| is where we stopped going up the chain earlier. It is
          // the point at which the chain of gets begins, the reference that
          // starts everything. In the new sequence we generate we need to keep
          // it at the top.
          auto* result = currValue;

          // Starting from the top, build up the new stack of instructions.
          for (Index i = 0; i < best->size(); i++) {
            auto item = (*best)[best->size() - i - 1];

            if (item.isGetIndex()) {
              auto index = item.getGetIndex();
              auto type = result->type.getHeapType().getStruct().fields[index].type;
              result = builder.makeStructGet(index, result, type);
            } else {
              assert(item.isCastType());
              result = builder.makeRefCast(result, item.getCastType(), RefCast::Safe);
            }
          }

          replaceCurrent(result);

          // TODO: We currently stop if we find an improvement at any sequence
          //       length. However, we could find how much we improve at different
          //       lengths - perhaps at a longer length we can remove more.
          return;
        }
      }
    }

    // A better sequence is either shorter, or it uses lower indexes. We also do
    // not want to ever add a cast (a cast involves at least a few memory
    // accesses and branching, unlike a plain load).
    bool isBetter(const Sequence& a, const Sequence& b) {
      if (a.size() < b.size()) {
        return true;
      }
      if (a.size() > b.size()) {
        return false;
      }

      // The size is equal, so consider the contents: Fewer casts is better.
      // TODO: Maybe allow adding a cast if it removes several other operations?
      auto aCasts = getNumCasts(a);
      auto bCasts = getNumCasts(b);
      if (aCasts < bCasts) {
        return true;
      }
      if (aCasts > bCasts) {
        return false;
      }

      // Same size and same casts, so prefer lower indexes.
      return a < b;
    }

    Index getNumCasts(const Sequence& s) {
      Index ret = 0;
      for (auto x : s) {
        if (x.isCastType()) {
          ret++;
        }
      }
      return ret;
    }

  private:
    TypeImprovementMap& unifiedMap;
  };
};

} // anonymous namespace

Pass* createEquivalentFieldOptimizationPass() {
  return new EquivalentFieldOptimization();
}

} // namespace wasm
