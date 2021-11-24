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
// Unifies all itables into a dispatch function. That is, we assume that the
// various classes have itables like this:
//
//  (global $foo.itable (array.init $itable
//    (null) ;; no entry for this category
//    (struct.new $some.vtable ;; an entry of size 2 for this category,
//                             ;; with heap type $some.vtable
//      (ref.func $a)
//      (ref.func $b)
//    )
//    ..
//  ))
//
// Each index refers to what we call a "category", globally. That is, the 0th
// index in all itables refers to the first category, and so forth. For
// example, the first category may be the interface "Serializable"; however,
// there may also be more than one source interface per category, if the
// producer emitted them that way, in which case different heap types of the
// vtables indicate that.
//
// At each category index an itable has either a null or a vtable that contains
// function references.
//
// Then we create dispatch functions that look like this:
//
//  function itable$dispatch$N$T(itableIndex, args..) {
//    if (itableIndex == 42) {
//      foo(args..);
//    } else ..
//
// Each dispatch function handles one category N and one heap type T in that
// category, and switches over the relevant
// itables that implement that category. For each such itable it does a direct
// call to its target.
//
// We also create functions that return whether an itable index supports a
// category,
//
//  function itable$supports$N$T(itableIndex) {
//    if (itableIndex == 42) {
//      return 1; // this itable supports this category
//    } else ..
//
// Assumption: The itable field is called $itable, and nothing else has that
// name; and likewise the itable array is called $itable.
//
// This may make DCE less productive, as DCE would need to know about which
// constant indices are used inside objects (which it does not atm). For that
// reason this pass makes sense as a final transformation.
//

#include "cfg/Relooper.h"
#include "ir/local-graph.h"
#include "ir/module-utils.h"
#include "ir/names.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-type.h"
#include "wasm.h"

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
  Module* module;

  std::unique_ptr<Builder> builder;

  void run(PassRunner* runner, Module* module_) override {
    module = module_;

    builder = std::make_unique<Builder>(*module);

    // Process itable data in globals and compute the layout of the unified
    // table. This also applies those changes to code.
    mapFunctionsToTable(runner, *module);

    // Replace $itable function fields in structs with i32s. This just does the
    // type changes.
    updateFieldTypes(*module);
  }

  // Fill in mapping info that describes how we lay out the dispatch table.
  struct MappingInfo {
    struct VTable {
      // The type of the table. The old code casts to this before loading, and
      // the new code needs this to tell if an itable supports an interface.
      HeapType type;

      // A list of the names of the functions in the vtable.
      std::vector<Name> funcs;
    };

    struct ITable {
      // The name of the global where this itable is defined.
      Name name;

      // An itable maps the category indexes that it implements to the vtable
      // for them.
      std::unordered_map<Index, VTable> vtables;

      ITable(Name name) : name(name) {}
    };

    // The itables in the wasm, in the order they appear. The index of an
    // itable in this vector is the index it is identified by.
    std::vector<ITable> itables;

    // A global mapping of heap types of vtables to indexes.
    std::unordered_map<HeapType, Index> typeIndexes;

    // A map of each category to its types.
    InsertOrderedMap<Index, InsertOrderedSet<HeapType>> categoryTypes;
  } mapping;

  void mapFunctionsToTable(PassRunner* runner, Module& wasm) {
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
      mapping.itables.emplace_back(global->name);
    });
//    auto numItables = mapping.itables.size();

    // Find the itable data.
    for (auto& itable : mapping.itables) {
      auto* global = wasm.getGlobal(itable.name);
      auto& itableOperands = global->init->cast<ArrayInit>()->values;
      Index category = 0;
      for (auto* operand : itableOperands) {
        // Elements in an itable are either a null or a struct.new.
        if (operand->is<RefNull>()) {
          // Nothing to do: this itable does not support this category.
        } else if (auto* new_ = operand->dynCast<StructNew>()) {
          // This itable supports this category. Create the vtable data
          // structure and set the type and function names.
          auto& vtable = itable.vtables[category];
          auto type = operand->type.getHeapType();
          vtable.type = type;
          for (auto* operand : new_->operands) {
            vtable.funcs.push_back(operand->cast<RefFunc>()->func);
          }

          // Index the type if we haven't already seen it.
          if (!mapping.typeIndexes.count(type)) {
            auto index = mapping.typeIndexes.size();
            mapping.typeIndexes[type] = index;
          }

          // Note the type is used in the category.
          mapping.categoryTypes[category].insert(type);
        } else {
          WASM_UNREACHABLE("bad array.init operand");
        }
        category++;
      }
    }

    // Generate the functions.
    for (auto& [category, types] : mapping.categoryTypes) {
      for (auto type : types) {
        generateSupportsFunc(category, type);
        //generateDispatchFunc(category, type);
      }
    }

    // Update the itable globals to contain indexes instead.
    for (Index index = 0; index < mapping.itables.size(); index++) {
      auto& itable = mapping.itables[index];
      auto* global = wasm.getGlobal(itable.name);
      global->init = builder->makeConst(index);
      global->type = Type::i32;
    }

#if 0
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
            builder.makeConst(int32_t(mapping.itableSize))));
        }
      }
    };

    CodeUpdater updater(mapping);
    updater.run(runner, &wasm);
    updater.walkModuleCode(&wasm);
#endif
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

private:
  void generateSupportsFunc(Index category, HeapType type) {
    std::map<Index, Expression*> indexToCode;
    for (Index itableIndex = 0; itableIndex < mapping.itables.size(); itableIndex++) {
      auto& itable = mapping.itables[itableIndex];
      // This itable implements this category+type if it has the category, and
      // if the type on that category is correct.
      if (itable.vtables.count(category) && itable.vtables[category].type == type) {
        indexToCode[itableIndex] =
          builder->makeReturn(
            builder->makeConst(int32_t(1))
          )
        ;
      }
    }

    // Switch over the things that support the category, each of which has a
    // return of 1 set up for it. Otherwise, return 0.
    auto* body = makeSwitch(
      indexToCode,
      builder->makeReturn(
        builder->makeConst(int32_t(0))
      ),
      builder->makeLocalGet(0, Type::i32)
    );

    module->addFunction(
      builder->makeFunction("itable$supports$" + std::to_string(category) + '$' + std::to_string(mapping.typeIndexes[type]),
                            Signature({Type::i32}, {Type::i32}),
                            {},
                            body)
    );
  }

#if 0
  void generateDispatchFunc(Index category) {
    // TODO: check if they have different signatures...
    std::map<Index, Expression*> indexToCode;
    for (Index index = 0; index < mapping.itables.size(); index++) {
      auto& itable = mapping.itables[index];
      if (itable.vtables.count(category)) {
        indexToCode.insert(
          builder->makeReturn(
            builder->makeConst(int32_t(1))
          )
        );
      }
    }
    // Switch over the things that support the category, each of which has a
    // return of 1 set up for it. Otherwise, return 0.
    body = makeSwitch(
      indexToCode,
      builder->makeReturn(
        builder->makeConst(int32_t(0))
      ),
      builder->makeLocalGet(0, Type::i32)
    );

    module->addFunction(
      builder->makeFunction("itable$supports$" + std::to_string(category),
                            Signature({Type::i32}, {Type::i32}),
                            {},
                            body)
    );
  }
#endif

  // This takes a map of indexes to the code we want to execute for that index,
  // and the code we want to execute in the default case when none of the
  // indexes is equal to the value at runtime. It also receives the value to
  // perform the switch on.
  Expression* makeSwitch(const std::map<Index, Expression*>& indexToCode, Expression* default_, Expression* condition) {
    CFG::Relooper relooper(module);

    // The entry block contains a switch on the condition, and nothing else.
    auto* entry = relooper.AddBlock(builder->makeNop(), condition);

    // Create blocks for each index. (Rely on the relooper to merge and optimize
    // them etc.)
std::cout << "reloop\n";

    for (auto& [index, code] : indexToCode) {
      auto* block = relooper.AddBlock(code);
std::cout << "block " << index << " => " << *code << '\n';
      entry->AddSwitchBranchTo(block, {index});
    }

std::cout << "default => " << *default_ << '\n';
    auto* defaultBlock = relooper.AddBlock(default_);
    entry->AddBranchTo(defaultBlock, nullptr);

    // Do not provide an index for a "label helper" local. We should never need
    // one. If we use one somehow, we'll error on it being an invalid local
    // index.
    CFG::RelooperBuilder relooperBuilder(*module, Index(-1));
    relooper.Calculate(entry);
    auto* result = relooper.Render(relooperBuilder);
    ReFinalize().walk(result);
    return result;
  }
};

} // anonymous namespace

Pass* createUnifyITablePass() { return new UnifyITable(); }

} // namespace wasm
