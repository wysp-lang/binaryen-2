/*
 * Copyright 2021 WebAssembly Community Group participants
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
// Converts vtables - structs of function references - to use indexes. That is,
// this replaces function reference fields with i32 fields. Specifically,
//
//  (struct (field (ref $functype1)) (field (ref $functype2))
// =>
//  (struct (field (ref i32))        (field (ref i32))
//
// This also creates a table for each field and populates it with the possible
// values. Then struct.news are altered to replace references with indexes, and
// struct.gets are altered to load from the table.
//
// Assumptions:
//  * All function reference fields are to be transformed.
//  * Such fields must be written to during creation of a vtable instance, and
//    with a constant ref.func.
//  * Vtable subtyping is allowed, but not to specialize types of the parent. If
//    that were done, we'd need to add casts to handle the table no having the
//    specialized type (it would have the subtype).
//

#include <ir/module-utils.h>
#include "ir/subtypes.h" // Needed?
#include <pass.h>
#include <wasm.h>
#include <wasm-type.h>

using namespace std;

namespace wasm {

namespace {

#if 0
struct FunctionInfoScanner
  : public WalkerPass<PostWalker<FunctionInfoScanner>> {
  bool isFunctionParallel() override { return true; }

  FunctionInfoScanner(NameInfoMap* infos) : infos(infos) {}

  FunctionInfoScanner* create() override {
    return new FunctionInfoScanner(infos);
  }

  void visitLoop(Loop* curr) {
    // having a loop
    (*infos)[getFunction()->name].hasLoops = true;
  }
};
#endif

struct VTableToIndexes : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // First, rewrite the types to replace function references with i32s.
    std::vector<HeapType> types;
    std::unordered_map<HeapType, Index> typeIndices;
    ModuleUtils::collectHeapTypes(*module, types, typeIndices);

    std::unordered_map<HeapType, Index> typeToIndex;
    for (Index i = 0; i < types.size(); i++) {
      typeToIndex[types[i]] = i;
    }

    TypeBuilder typeBuilder(types.size());

    auto getNewType = [&](Type type, bool isStructField) {
      if (type.isBasic()) {
        return type;
      }
      if (type.isReference()) {
        return typeBuilder.getTempRefType(
          typeBuilder.getTempHeapType(typeToIndex.at(type)),
          type.getNullability()
        );
      }
      if (type.isRtt()) {
        auto rtt = type.getRtt();
        auto newRtt = rtt;
        newRtt.heapType = 
          typeBuilder.getTempHeapType(typeToIndex.at(type));
        return typeBuilder.getTempRttType(newRtt);
      }
      if (type.isTuple()) {
        auto& tuple = type.getTuple();
        auto newTuple = tuple;
        for (auto& t : newTuple) {
          t = getNewType(t);
        }
        return typeBuilder.getTempTupleType(newTuple);
      }
      WASM_UNREACHABLE("bad type");
    };

    auto getNewTypeForStruct = [&](Type type) {
      if (type.isFunction()) {
        // This is exactly what we are looking to change!
        return Type::i32;
      }
      return getNewType(type);
    };

    for (Index i = 0; i < types.size(); i++) {
      auto type = types[i];
      if (type.isSignature()) {
        auto sig = type.getSignature();
        TypeList newParams, newResults;
        for (auto t : sig.params) {
          newParams.push_back(getNewType(t));
        }
        for (auto t : sig.results) {
          newResults.push_back(getNewType(t));
        }
        typeBuilder.setHeapType(i, Signature{newParams, newResults};
      } else if (type.isStruct()) {
        auto struct_ = type.getStruct();
        // Start with a copy to get mutability/packing/etc.
        auto newStruct = struct_;
        for (auto& field : newStruct.fields) {
          field.type = getNewTypeForStruct(field.type);
        }
        typeBuilder.setHeapType(i, newStruct);
      } else if (type.isArray()) {
        auto array = type.getArray();
        // Start with a copy to get mutability/packing/etc.
        auto newArray = array;
        newArray.element = getNewType(newArray.element);
        typeBuilder.setHeapType(i, newArray);
      } else {
        WASM_UNREACHABLE("bad type");
      }
    }



    //SubTypes subTypes(*module);
    //FunctionInfoScanner().run(runner, module);
  }
};

} // anonymous namespace

Pass* createVTableToIndexesPass() { return new VTableToIndexes(); }

} // namespace wasm
