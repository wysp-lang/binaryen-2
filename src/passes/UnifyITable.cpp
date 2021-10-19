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
//    (null) (null) ..            ;; as many nulls as that first category needs
//    (ref.func $a) (ref.func $b) ;; the funcs from the second category (plus
//                                ;; nulls, to fill the category, see below)
//  )
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
    // Create the unified table.
    auto unifiedTable =
      Names::getValidTableName(*getModule(), "unified-table");
    wasm.addTable(Builder::makeTable(unifiedTable, Type::funcref));
    auto segmentName = Names::getValidElementSegmentName(
      wasm, unifiedTable.str + std::string("$segment"));
    auto* segment = wasm.addElementSegment(Builder::makeElementSegment(
      segmentName,
      unifiedTable,
      Builder(*getModule()).makeConst(int32_t(0)),
      Type::funcref));
    auto& segmentData = segment->data;

    // Fill in mapping info that describes how we lay out the unified table.
    struct MappingInfo {
      // The size of each category.
      std::vector<Index> categorySizes;

      // The list of itable globals, in the order they appear in the wasm.
      std::vector<Name> itables;

      // Maps the names of itable globals in the old model to base offsets in
      // the unified table in the new model.
      std::unordered_map<Name, Index> itableToBase;


    } mapping;

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
      mapping.categorySizes.resize(index + 1);
      mapping.categorySizes[index] = std::max(mapping.categorySizes[index], size);
    };

    for (auto itable : mapping.itables) {
      auto itableGlobal = wasm.getGlobal(itable);
      auto itableOperands = itableGlobal->init->cast<ArrayInit>()->operands;
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
    // sizes;
    Index itableSize = 0;
    for (auto size : mapping.categorySizes) {
      itableSize += size;
    }

    // The unified table's segment's size is now known.
    segmentData.resize(itableSize * numItables);

    // Now that we know the sizes of the categories, we can generate the layout
    // of the unified table and fill it out.
    Builder builder(wasm);
    Index tableIndex = 0;
    for (auto itable : mapping.itables) {
      auto itableGlobal = wasm.getGlobal(itable);
      auto itableOperands = itableGlobal->init->cast<ArrayInit>()->operands;

      // Pick the base for this itable.
      mapping.itableToBase[itable] = tableIndex;

      auto categoryIndex = 0;
      for (auto* operand : itableOperands) {
        auto categorySize = mapping.categorySizes[categoryIndex];
        if (operand->is<RefNull>()) {
          // Fill the entire category with nulls.
          for (Index i = 0; i < categorySize; i++) {
            segmentData[tableIndex++] = builder.makeRefNull(HeapType::func);
          }
        } else if (auto* new_ = operand->dynCast<StructNew>()) {
          // Copy in the contents.
          for (auto* newOperand : new_->operands) {
            auto* refFunc = newOperand->cast<RefFunc>();
            segmentData[tableIndex++] = builder.makeRefFunc(refFunc->name, refFunc->type);
          }

          // Fill the remaining space with nulls.
          for (Index i = 0; i < categorySize - new_->operands.size(); i++) {
            segmentData[tableIndex++] = builder.makeRefNull(HeapType::func);
          }
        } else {
          WASM_UNREACHABLE("bad array.init operand");
        }
        categoryIndex++;
      }
    }
    
    assert(tableIndex == segmentData.size());
    assert(tableIndex == itableSize * numItables);
    
    // Update the code in the entire module.
    struct CodeUpdater : public WalkerPass<PostWalker<CodeUpdater>> {
      bool isFunctionParallel() override { return true; }

      MappingInfo& mapping;

      CodeUpdater(MappingInfo& mapping) : mapping(mapping) {}

      CodeUpdater* create() override { return new CodeUpdater(mapping); }

      void visitStructNew(StructNew* curr) {
        // Where we used to write a global.get of an itable, instead write out
        // the base of that itable's data in the unified table.
        for (auto*& operand : curr->operands) {
          if (auto* get = operand->dynCast<GlobalGet>()) {
            auto iter = mapping.itableToBase.find(get->name);
            if (iter != mapping.itableToBase.end()) {
              replaceCurrent(Builder(*getModule()).makeConst(iter->second);
            }
          }
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
        auto* objectGet = arrayGet->target->dynCast<StructGet>();
        if (!objectGet) {
          return;
        }
        // TODO: we'll need global.get of an itable here, once CFP does that.

        // The shape fits, check that it all begins with a read of an itable.
        auto objectType = objectGet->ref->type.getHeapType();
        auto& typeNames = getModule()->typeNames;
        if (!typeNames.count(objectType) ||
            !typeNames[objectType].fields.count(objectGet->index) ||
            typeNames[objectType].fields[objectGet->index] != ITABLE) {
          // No name for this field, or the name is not "itable".
          return;
        }
      }

      Name getFieldTable(HeapType type, Index i) {
        auto& fieldTable = mapping.fieldTables[{type, i}];
        if (!fieldTable.is()) {
          // Compute the table in which we will store functions for this field.
          // First, find the supertype in which this field was first defined;
          // all subclasses use the same table for their functions.
          // TODO: more memoization here
          HeapType parent = type;
          while (1) {
            HeapType grandParent;
            if (!parent.getSuperType(grandParent)) {
              // No more supers, so parent is the topmost one.
              break;
            }
            if (i >= grandParent.getStruct().fields.size()) {
              // The grand-parent does not have this field, so parent is where
              // it is first defined.
              break;
            }
            // Otherwise, continue up.
            parent = grandParent;
          }

          // We know the proper supertype, and our table is the one it has.
          auto& parentFieldTable = mapping.fieldTables[{parent, i}];
          if (!parentFieldTable.is()) {
            // This is the first time we need a table for this parent; do so
            // now.
            parentFieldTable =
              Names::getValidTableName(*getModule(), "v-table");
            auto fieldType = type.getStruct().fields[i].type;
            if (fieldType.isNonNullable()) {
              // Non-nullable types are not allowed in tables yet.
              fieldType = Type(fieldType.getHeapType(), Nullable);
            }
            getModule()->addTable(
              Builder::makeTable(parentFieldTable, fieldType));
            Name segmentName = Names::getValidElementSegmentName(
              *getModule(), parentFieldTable.str + std::string("$segment"));
            getModule()->addElementSegment(Builder::makeElementSegment(
              segmentName,
              parentFieldTable,
              Builder(*getModule()).makeConst(int32_t(0)),
              fieldType));
            mapping.tableInfos[parentFieldTable].segmentName = segmentName;
          }

          // Copy from the parent;
          fieldTable = parentFieldTable;
        }

        return fieldTable;
      }

      // Returns the index of a function in a table. If not already present
      // there, this allocates a new entry in the table.
      Index getFuncIndex(Name tableName, Name func) {
        auto& tableInfo = mapping.tableInfos[tableName];
        auto& funcIndexes = tableInfo.funcIndexes;
        if (funcIndexes.count(func)) {
          return funcIndexes[func];
        }

        // Enlarge the table, add to the segment, and update the info.
        auto index = funcIndexes.size();
        funcIndexes[func] = index;
        auto* table = getModule()->getTable(tableName);
        table->initial = table->max = index + 1;
        auto* segment = getModule()->getElementSegment(tableInfo.segmentName);
        segment->data.push_back(
          Builder(*getModule())
            .makeRefFunc(func, getModule()->getFunction(func)->type));
        return index;
      }
    };

    CodeUpdater updater(mappingInfo);
    updater.run(runner, &wasm);
    updater.walkModuleCode(&wasm);
  }

  void updateFieldTypes(Module& wasm) {
    class TypeRewriter : public GlobalTypeRewriter {
    public:
      TypeRewriter(Module& wasm) : GlobalTypeRewriter(wasm) {}

      virtual void modifyStruct(HeapType oldStructType, Struct& struct_) {
        auto& oldFields = oldStructType.getStruct().fields;
        auto& newFields = struct_.fields;

        for (Index i = 0; i < oldFields.size(); i++) {
          // Check for function-hood on the old fields, as the new ones contain
          // temp types that we should not be accessing.
          if (oldFields[i].type.isFunction()) {
            // This is exactly what we are looking to change!
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
