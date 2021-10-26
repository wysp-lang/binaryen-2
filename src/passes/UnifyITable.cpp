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

#include <ir/local-graph.h>
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
    // Fill in mapping info that describes how we lay out the dispatch table.
    struct MappingInfo {
      // The name of the unified dispatch table that we will create and use
      // everywhere. This table contains function references.
      Name dispatchTable;

      // The name of the unified test table that we will create and use
      // everywhere. This table contains vtables, and is used by ref.test to
      // basically check if an object supports an interface.
      // TODO: remove this if j2cl can stop using vtables this way
      Name testTable;

      // The list of itable globals, in the order they appear in the wasm.
      std::vector<Name> itables;

      // Maps the names of itable globals in the old model to base offsets in
      // the dispatch table in the new model.
      std::unordered_map<Name, Index> itableBases;

      // The size of each category.
      std::vector<Index> categorySizes;

      // The total size of each itable, which is the sum of the category sizes.
      Index itableSize;

      // The base index of each category, that is, at what offset that category
      // begins (in each itable region in the dispatch table).
      std::vector<Index> categoryBases;
    } mapping;

    // Create the dispatch table.
    mapping.dispatchTable = Names::getValidTableName(wasm, "dispatch-table");
    wasm.addTable(Builder::makeTable(mapping.dispatchTable, Type::funcref));
    auto segmentName = Names::getValidElementSegmentName(
      wasm, mapping.dispatchTable.str + std::string("$segment"));
    Builder builder(wasm);
    auto* segment = wasm.addElementSegment(
      Builder::makeElementSegment(segmentName,
                                  mapping.dispatchTable,
                                  builder.makeConst(int32_t(0)),
                                  Type::funcref));
    auto& segmentData = segment->data;

    // Find the itable globals.
    ModuleUtils::iterDefinedGlobals(wasm, [&](Global* global) {
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
      mapping.categorySizes[index] =
        std::max(mapping.categorySizes[index], size);
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
    mapping.itableSize = itableSize;

    // The dispatch table's segment's size is now known.
    segmentData.resize(itableSize * numItables);

    // Now that we know the sizes of the categories, we can generate the layout
    // of the dispatch table and fill it out.
    Index tableIndex = 0;
    for (auto itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable);
      auto& itableOperands = global->init->cast<ArrayInit>()->values;

      // Pick the base for this itable.
      mapping.itableBases[itable] = tableIndex;

      for (Index categoryIndex = 0;
           categoryIndex < mapping.categorySizes.size();
           categoryIndex++) {
        auto categorySize = mapping.categorySizes[categoryIndex];
        // This category might not exist in this itable. If not, we just need
        // nulls there.
        auto* operand = categoryIndex < itableOperands.size()
                          ? itableOperands[categoryIndex]
                          : nullptr;
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
            segmentData[tableIndex++] =
              builder.makeRefFunc(refFunc->func, refFunc->type.getHeapType());
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
    auto totalTableSize = tableIndex;

    // Update the table sizes.
    auto* table = wasm.getTable(mapping.dispatchTable);
    table->initial = totalTableSize;
    table->max = totalTableSize;

    // Create the test table, which is an array of datas, now that we know its
    // size. It begins initialized with nulls, and a start function will assign
    // values to it. (Note that we can't use array.init as it is far too large
    // due to VM limitations!)
    auto testTableType = Type(
      Array(Field(Type(HeapType::data, Nullable), Mutable)), Nullable);
    mapping.testTable = Names::getValidGlobalName(wasm, "test-table");
    wasm.addGlobal(Builder::makeGlobal(
      mapping.testTable, testTableType, builder.makeRefNull(testTableType), Builder::Mutable));

    // Create a start function for the test table assignments.
    // TODO: handle an existing start function by prepending.
    assert(!wasm.start.is());
    auto startName = Names::getValidFunctionName(wasm, "start");
    auto* startBlock = builder.makeBlock();
    wasm.addFunction(builder.makeFunction(startName,
                                          Signature(Type::none, Type::none),
                                          {},
                                          startBlock));
    wasm.start = startName;

    // Create the test table. (V8 atm does not allow array.new in globals.)
    startBlock->list.push_back(
      builder.makeGlobalSet(
        mapping.testTable,
        builder.makeArrayNew(
          testTableType.getHeapType(),
          builder.makeConst(uint32_t(totalTableSize))
        )
      )
    );

    // Update the itable globals to contain offsets instead. That way when the
    // globals are read in order to initialize the object's $itable fields, we
    // will get the proper values.
    for (auto itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable);
      auto* oldInit = global->init;
      auto itableBase = mapping.itableBases[itable];
      global->init = Builder(wasm).makeConst(itableBase);
      global->type = Type::i32;

      // The old init is placed in the test table, where it can be used by
      // ref.test instructions.
      startBlock->list.push_back(
        builder.makeArraySet(
          builder.makeGlobalGet(
            mapping.testTable,
            testTableType
          ),
          builder.makeConst(uint32_t(itableBase)),
          oldInit
        )
      );
    }

    // Update the code in the entire module.
    struct CodeUpdater : public WalkerPass<PostWalker<CodeUpdater>> {
      bool isFunctionParallel() override { return true; }

      MappingInfo& mapping;

      CodeUpdater(MappingInfo& mapping) : mapping(mapping) {}

      CodeUpdater* create() override { return new CodeUpdater(mapping); }

      // Adapt to the change of struct $itable fields now contain an i32 instead
      // of a reference. That is, we have things like this:
      //
      //  (struct.new ..
      //    (global.get $some-itable)
      //
      // and we need to write the proper i32 there instead. Note that the itable
      // may not be read and immediately written out as it is here: if it is
      // used multiple times in a function it may be cached in a local.
      //
      // The simple case is handled by our replacing the global's value with a
      // ref to contain the i32. For that, we just need to update the type of
      // relevant global.gets.
      //
      // Aside from that, we need to handle the case where the itable value
      // is passed through locals (which it might if it was read multiple times
      // and ended up CSEed into a local). Furthermore, we need to handle reads
      // from the struct field, that now return an i32, and those reads will end
      // up used in an itable call pattern like this:
      //
      //  (call_ref
      //   (struct.get $vtable $vtable.field
      //    (ref.cast $vtable
      //     (array.get $itable
      //      (struct.get $object $itable  ;; This is where the itable arrives
      //       (..ref..)
      //      )
      //      (i32.const itable-offset)
      //
      // As with the global value being passed through a local, any of the
      // steps here might, as well, depending on CSE and other opts. To handle
      // all of that, we do this:
      //
      //  * We detect when an itable value arrives. It comes from either a
      //    global.get or struct.get as described earlier.
      //  * We mark such arrivals as |inPattern|, and we use that information
      //    in anything that accesses them, which includes their parents as well
      //    as flowing through locals. Those uses are then marked as also being
      //    in |inPattern|, and are altered accordingly (see details below).

      std::unordered_set<Expression*> inPattern;

      void visitGlobalGet(GlobalGet* curr) {
        if (mapping.itableBases.count(curr->name)) {
          inPattern.insert(curr);
          curr->type = Type::i32;
        }
      }

      // Map struct.gets in our pattern to the types they used to return. These
      // struct.gets read a function reference from the specific vtable in the
      // itable, so they contain knowledge of the function type we should be
      // calling. Note that as we rewrite the struct.get we lose that
      // information, as it will now return an i32 (and also be replaced with
      // an entirely new instruction), which is why we must stash it on the side
      // here so that the parent can find the type.
      std::unordered_map<Expression*, Type> oldStructGetTypes;

      void visitStructGet(StructGet* curr) {
        // This may be a struct.get from the vtable in the itable pattern,
        //   struct.get $vtable $vtable.field
        // in the comment from earlier. We detect that if the reference is
        // known to be in the pattern. If it is, then so are we.
        if (inPattern.count(curr->ref)) {
          // The vtable read is at an offset which we need to add to the value
          // so far.
          Builder builder(*getModule());
          auto* add = builder.makeBinary(
            AddInt32, curr->ref, builder.makeConst(int32_t(curr->index)));
          replaceCurrent(add);
          inPattern.insert(add);
          oldStructGetTypes[add] = curr->type;
          return;
        }

        // Or, this may be where an itable value arrives, if it is read from
        // an itable field.
        if (isItableField(
              curr->ref->type.getHeapType(), curr->index, *getModule())) {
          inPattern.insert(curr);
          curr->type = Type::i32;
        }
      }

      // The case where the value flows through a local requires us to do a
      // local analysis.
      std::unique_ptr<LocalGraph> localGraph;

      void visitLocalSet(LocalSet* curr) {
        auto* func = getFunction();

        if (inPattern.count(curr->value)) {
          inPattern.insert(curr);

          // TODO: handle itables stored in params..?
          assert(func->isVar(curr->index));

          // Update the var's type.
          func->vars[curr->index - func->getVarIndexBase()] = Type::i32;

          // If this is a tee, its type changes as well.
          if (curr->isTee()) {
            curr->makeTee(Type::i32);
          }

          // Update all gets.
          if (!localGraph) {
            localGraph = std::make_unique<LocalGraph>(func);
            localGraph->computeSetInfluences();
          }
          for (auto* get : localGraph->setInfluences[curr]) {
            get->type = Type::i32;

            // Mark them as well, so that their parents know to update
            // themselves.
            inPattern.insert(get);
          }
        }
      }

      void visitArrayGet(ArrayGet* curr) {
        if (inPattern.count(curr->ref)) {
          inPattern.insert(curr);

          // Our reference is an itable base. We need to add the category
          // offset to it.
          Index categoryIndex = curr->index->cast<Const>()->value.geti32();
          assert(categoryIndex < mapping.categoryBases.size());
          auto categoryBase = mapping.categoryBases[categoryIndex];
          Builder builder(*getModule());
          auto* add = builder.makeBinary(
            AddInt32, curr->ref, builder.makeConst(int32_t(categoryBase)));
          replaceCurrent(add);
          inPattern.insert(add);
        }
      }

      void visitRefCast(RefCast* curr) {
        if (inPattern.count(curr->ref)) {
          // The cast is no longer needed; skip it.
          replaceCurrent(curr->ref);

          // TODO: handle rtts with side effects?
          assert(!curr->rtt || curr->rtt->is<RttCanon>());
        }
      }

      void visitRefTest(RefTest* curr) {
        if (inPattern.count(curr->ref)) {
          // Do a test on the test table's contents at the relevant location.
          Builder builder(*getModule());
          auto* globalGet = builder.makeGlobalGet(
            mapping.testTable, getModule()->getGlobal(mapping.testTable)->type);
          curr->ref = builder.makeArrayGet(globalGet, curr->ref);
        }
      }

      void visitRefAs(RefAs* curr) {
        if (inPattern.count(curr->value)) {
          // This is a ref.as_non_null of an itable in a local (which is now
          // an i32). Skip the ref_as.
          assert(curr->op == RefAsNonNull);
          replaceCurrent(curr->value);
        }
      }

      void visitCallRef(CallRef* curr) {
        if (inPattern.count(curr->target)) {
          // We have reached the end of the pattern: our call target contains
          // not a function reference but an index in the dispatch table,
          // including all necessary offseting. All that we have left to do is
          // to replace the call_ref with an appropriate call_indirect. To do
          // so, we need to know the proper signature. It is *not* valid to
          // infer that signature from the parameters passed to this CallRef,
          // since they may be subtypes of the actual function being called.
          // Instead, we use our knowledge of what function type the struct.get
          // that used to be our child had.
          auto funcType = oldStructGetTypes[curr->target];
          auto sig = funcType.getHeapType().getSignature();
          auto* call =
            Builder(*getModule())
              .makeCallIndirect(
                mapping.dispatchTable, curr->target, curr->operands, sig);
          replaceCurrent(call);
        }
      }

      void visitArrayLen(ArrayLen* curr) {
        if (inPattern.count(curr->ref)) {
          // This code checks for the size of an itable, which is done before
          // doing a ref.test on it. XXX give the maximum length possible among
          // all itables, which is not precisely accurate but as this is just
          // used to avoid a trap, good enough: if the itable is shorter then
          // we'll load a null and the ref.test will fail anyhow.
          Builder builder(*getModule());
          replaceCurrent(builder.makeSequence(
            builder.makeDrop(curr->ref),
            builder.makeConst(int32_t(mapping.itableSize))
          ));
        }
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
