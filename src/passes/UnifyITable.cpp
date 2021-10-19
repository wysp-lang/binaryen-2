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
// Unifies all itables into a single universal global dispatch table. That is,
// if various classes have itables like this:
//
//  (global $foo.itable (array.init $itable
//    (null) ;; no entry for this category
//    (struct.new $some.vtable ;; an entry of size 2 for this category
//      (ref.func $a)
//      (ref.func $b)
//    )
//    ..
//  ))
//
// Each index refers to what we call a "category", globally. That is, the 0th
// index in all itables refers to the first category, and so forth. (For
// example, the first category may be the interface "Serializable"; however,
// there may also be more than one source interface per category, if the
// producer emitted them that way.) At each index, an itable has either a null
// or a vtable that contains function references.
//
// Then we create a single table to rule them all ("the one table"):
//
//  (table $unified-table
//    ;; first itable
//    (null) (null) ..            ;; as many nulls as that first category needs
//    (ref.func $a) (ref.func $b) ;; the funcs from the second category (plus
//                                ;; nulls, to fill the category, see below)
//    ;; second itable, etc.
//  )
// TODO: better graph
//
// Fields that were (ref $itable) become i32s, where that value is the offset
// into the one table, which indicates where that class's info begins. To that
// we add:
//
//  * The itable category offset, which in the old model was the constant
//    offset in the array.get, and which now becomes a global "category size"
//    constant. All categories are forced to the same size.
//  * The vtable offset. This is simply the index in the vtable that the
//    struct.get used.
//
// Assuming the itable category is constant, this means we replace something
// like
//
//  (call_ref
//   (struct.get from vtable
//    (ref.cast to vtable
//     (array.get from itable
//      (struct.get to get the itable
//
// with
//
//  (call_indirect
//   (i32.add with a constant (both itable category and vtable offset)
//    (struct.get to get the class base in the one table
//
// That is, we replace a cast, a struct.get, and an array.get with an add. (We
// also replace a call_ref with a call_indirect, which may be slightly less
// efficient, actually, and is worth investigation.)
//
// Assumption: The itable field is called $itable, and nothing else has that
// name; and likewise the itable array is called $itable.
//
// This may make DCE less productive, as DCE would need to know about which
// constant indices are used inside objects (which it does not atm). For that
// reason this pass makes sense as a final transformation.
//

#include <ir/module-utils.h>
#include <ir/names.h>
#include <ir/type-updating.h>
#include <ir/utils.h>
#include <pass.h>
#include <wasm-builder.h>
#include <wasm-type.h>
#include <wasm.h>

using namespace std;

namespace wasm {

namespace {

static const Name ITABLE("itable");

static bool isItableField(HeapType type, Index fieldIndex, Module& wasm) {
  auto& typeNames = wasm.typeNames;
  return typeNames.count(type) &&
         typeNames[type].fieldNames.count(fieldIndex) &&
         typeNames[type].fieldNames[fieldIndex] == ITABLE;
}

struct UnifyITable : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // Process itable data in globals and compute the layout of the unified
    // table. This also applies those changes to code.
    mapFunctionsToTable(runner, *module);

    // Replace $itable function fields in structs with i32s. This just does the
    // type changes.
    updateFieldTypes(*module);
  }

  void mapFunctionsToTable(PassRunner* runner, Module& wasm) {
    // Fill in mapping info that describes how we lay out the unified table.
    struct MappingInfo {
      // The name of the unified table that we will create and use everywhere.
      Name unifiedTable;

      // The list of itable globals, in the order they appear in the wasm.
      std::vector<Name> itables;

      // Maps the names of itable globals in the old model to base offsets in
      // the unified table in the new model.
      std::unordered_map<Name, Index> itableBases;

      // The size of each category.
      std::vector<Index> categorySizes;

      // The base index of each category, that is, at what offset that category
      // begins (in each itable region in the unified table).
      std::vector<Index> categoryBases;
    } mapping;

    // Create the unified table.
    mapping.unifiedTable =
      Names::getValidTableName(wasm, "unified-table");
    wasm.addTable(Builder::makeTable(mapping.unifiedTable, Type::funcref));
    auto segmentName = Names::getValidElementSegmentName(
      wasm, mapping.unifiedTable.str + std::string("$segment"));
    auto* segment = wasm.addElementSegment(Builder::makeElementSegment(
      segmentName,
      mapping.unifiedTable,
      Builder(wasm).makeConst(int32_t(0)),
      Type::funcref));
    auto& segmentData = segment->data;

    // Find the itable globals.
    ModuleUtils::iterDefinedGlobals(
      wasm, [&](Global* global) {
        // Itables are created with array.init and nothing else.
        if (!global->init->is<ArrayInit>()) {
          return;
        }

        auto type = global->init->type;
        if (!type.isArray()) {
          return;
        }
        if (wasm.typeNames[type.getHeapType()].name != ITABLE) {
          return;
        }

        // This is an itable.
        mapping.itables.push_back(global->name);
      });
    auto numItables = mapping.itables.size();

    // Compute the size of the categories.
    auto ensureCategorySize = [&](Index index, Index size) {
      if (index >= mapping.categorySizes.size()) {
        mapping.categorySizes.resize(index + 1);
      }
      mapping.categorySizes[index] = std::max(mapping.categorySizes[index], size);
    };

    for (auto itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable);
      auto& itableOperands = global->init->cast<ArrayInit>()->values;
      Index index = 0;
      for (auto* operand : itableOperands) {
        // Elements in an itable are either a null or a struct.new.
        if (operand->is<RefNull>()) {
          // Just ensure the category exists.
          ensureCategorySize(index, 0);
        } else if (auto* new_ = operand->dynCast<StructNew>()) {
          ensureCategorySize(index, new_->operands.size());
        } else {
          WASM_UNREACHABLE("bad array.init operand");
        }
        index++;
      }
    }

    // The size of an itable in the new layout is the sum of all the category
    // sizes. Calculate that and the base for each category.
    Index itableSize = 0;
    for (auto size : mapping.categorySizes) {
      mapping.categoryBases.push_back(itableSize);
      itableSize += size;
    }

    // The unified table's segment's size is now known.
    segmentData.resize(itableSize * numItables);

    // Now that we know the sizes of the categories, we can generate the layout
    // of the unified table and fill it out.
    Builder builder(wasm);
    Index tableIndex = 0;
    for (auto itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable);
      auto& itableOperands = global->init->cast<ArrayInit>()->values;

      // Pick the base for this itable.
      mapping.itableBases[itable] = tableIndex;

      for (Index categoryIndex = 0; categoryIndex < mapping.categorySizes.size(); categoryIndex++) {
        auto categorySize = mapping.categorySizes[categoryIndex];
        // This category might not exist in this itable. If not, we just need
        // nulls there.
        auto* operand = categoryIndex < itableOperands.size() ? itableOperands[categoryIndex] : nullptr;
        if (!operand || operand->is<RefNull>()) {
          // Fill the entire category with nulls.
          for (Index i = 0; i < categorySize; i++) {
            assert(tableIndex < segmentData.size());          
            segmentData[tableIndex++] = builder.makeRefNull(HeapType::func);
          }
        } else if (auto* new_ = operand->dynCast<StructNew>()) {
          // Copy in the contents.
          for (auto* newOperand : new_->operands) {
            auto* refFunc = newOperand->cast<RefFunc>();
            assert(tableIndex < segmentData.size());          
            segmentData[tableIndex++] = builder.makeRefFunc(refFunc->func, refFunc->type.getHeapType());
          }

          // Fill the remaining space with nulls.
          for (Index i = 0; i < categorySize - new_->operands.size(); i++) {
            assert(tableIndex < segmentData.size());          
            segmentData[tableIndex++] = builder.makeRefNull(HeapType::func);
          }
        } else {
          WASM_UNREACHABLE("bad array.init operand");
        }
      }
    }
    
    assert(tableIndex == segmentData.size());
    assert(tableIndex == itableSize * numItables);

    auto* table = wasm.getTable(mapping.unifiedTable);
    table->initial = tableIndex;
    table->max = tableIndex;

    // Update the globals to contain offsets instead. That way when the globals
    // are read in order to initialize the object's $itable fields, we will get
    // the proper values.
    for (auto itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable);
      global->init = Builder(wasm).makeConst(mapping.itableBases[itable]);
      global->type = Type::i32;
    }

    // Update the code in the entire module.
    struct CodeUpdater : public WalkerPass<PostWalker<CodeUpdater>> {
      bool isFunctionParallel() override { return true; }

      MappingInfo& mapping;

      CodeUpdater(MappingInfo& mapping) : mapping(mapping) {}

      CodeUpdater* create() override { return new CodeUpdater(mapping); }

      void visitGlobalGet(GlobalGet* curr) {
        if (mapping.itableBases.count(curr->name)) {
          // This global now contains an i32 base.
          curr->type = Type::i32;
        }
      }

      void visitCallRef(CallRef* curr) {
        if (curr->type == Type::unreachable) {
          return;
        }

        // We are looking for the particular pattern of
        //
        //  (call_ref
        //   (struct.get $vtable $vtable.field
        //    (ref.cast $vtable
        //     (array.get $itable
        //      (struct.get $object $itable
        //       (..ref..)
        //      )
        //      (i32.const itable-offset)
        //
        auto* vtableGet = curr->target->dynCast<StructGet>();
        if (!vtableGet) {
          return;
        }
        auto* refCast = vtableGet->ref->dynCast<RefCast>();
        if (!refCast) {
          return;
        }
        auto* arrayGet = refCast->ref->dynCast<ArrayGet>();
        if (!arrayGet) {
          return;
        }
        auto* objectGet = arrayGet->ref->dynCast<StructGet>();
        if (!objectGet) {
          return;
        }
        // TODO: we'll need global.get of an itable here, once CFP does that.

        // The shape fits, check that it all begins with a read of an itable.
        auto objectType = objectGet->ref->type.getHeapType();
        if (!isItableField(objectType, objectGet->index, *getModule())) {
          return;
        }

        // Perfect, this is the pattern we seek.
        Builder builder(*getModule());

        // Reuse the objectGet, which now loads the i32 base from the same field
        // that used to hold a reference to an itable.
        objectGet->type = Type::i32;

        // Compute the index in the unified table: add the base from the object
        // to the category base and the index in the category.
        Index categoryIndex = arrayGet->index->cast<Const>()->value.geti32();
        assert(categoryIndex < mapping.categoryBases.size());
        auto categoryBase = mapping.categoryBases[categoryIndex];
        auto indexInCategory = vtableGet->index;
        auto* target = builder.makeBinary(
          AddInt32,
          objectGet,
          builder.makeConst(int32_t(categoryBase + indexInCategory))
        );

        auto* call = builder.makeCallIndirect(mapping.unifiedTable,
                                              target,
                                              curr->operands,
                                              curr->target->type.getHeapType().getSignature());

        // TODO: handle rtts with side effects?
        assert(!refCast->rtt || refCast->rtt->is<RttCanon>());

        replaceCurrent(call);
      }
    };

    CodeUpdater updater(mapping);
    updater.run(runner, &wasm);
    updater.walkModuleCode(&wasm);
  }

  void updateFieldTypes(Module& wasm) {
    class TypeRewriter : public GlobalTypeRewriter {
    public:
      TypeRewriter(Module& wasm) : GlobalTypeRewriter(wasm) {}

      virtual void modifyStruct(HeapType oldStructType, Struct& struct_) {
        auto& newFields = struct_.fields;
        for (Index i = 0; i < newFields.size(); i++) {
          if (isItableField(oldStructType, i, wasm)) {
            newFields[i].type = Type::i32;
          }
        }
      }
    };

    TypeRewriter(wasm).update();
  }
};

} // anonymous namespace

Pass* createUnifyITablePass() { return new UnifyITable(); }

} // namespace wasm
