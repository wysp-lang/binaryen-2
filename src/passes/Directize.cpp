/*
 * Copyright 2019 WebAssembly Community Group participants
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
// Turn indirect calls into direct calls. This is possible if we know
// the table cannot change, and if we see a constant argument for the
// indirect call's index.
//

#include <unordered_map>

#include "ir/properties.h"
#include "ir/table-utils.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

namespace {

struct FunctionDirectizer : public WalkerPass<PostWalker<FunctionDirectizer>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new FunctionDirectizer(tables); }

  FunctionDirectizer(
    const std::unordered_map<Name, TableUtils::FlatTable>& tables)
    : tables(tables) {}

  void visitCallIndirect(CallIndirect* curr) {
    auto it = tables.find(curr->table);
    if (it == tables.end()) {
      return;
    }

    auto& flatTable = it->second;

    if (curr->target->is<StructGet>()) {
      optimizeITable(curr);
      return;
    }

    // If the target is constant, we can emit a direct call.
    if (curr->target->is<Const>()) {
      std::vector<Expression*> operands(curr->operands.begin(),
                                        curr->operands.end());
      replaceCurrent(makeDirectCall(operands, curr->target, flatTable, curr));
      return;
    }

    // If the target is a select of two different constants, we can emit two
    // direct calls.
    // TODO: handle 3+
    // TODO: handle the case where just one arm is a constant?
    if (auto* select = curr->target->dynCast<Select>()) {
      if (select->ifTrue->is<Const>() && select->ifFalse->is<Const>()) {
        Builder builder(*getModule());
        auto* func = getFunction();
        std::vector<Expression*> blockContents;

        // We must use the operands twice, and also must move the condition to
        // execute first; use locals for them all. While doing so, if we see
        // any are unreachable, stop trying to optimize and leave this for DCE.
        std::vector<Index> operandLocals;
        for (auto* operand : curr->operands) {
          if (operand->type == Type::unreachable ||
              !TypeUpdating::canHandleAsLocal(operand->type)) {
            return;
          }
          auto currLocal = builder.addVar(func, operand->type);
          operandLocals.push_back(currLocal);
          blockContents.push_back(builder.makeLocalSet(currLocal, operand));
        }

        if (select->condition->type == Type::unreachable) {
          return;
        }

        // Build the calls.
        auto numOperands = curr->operands.size();
        auto getOperands = [&]() {
          std::vector<Expression*> newOperands(numOperands);
          for (Index i = 0; i < numOperands; i++) {
            newOperands[i] =
              builder.makeLocalGet(operandLocals[i], curr->operands[i]->type);
          }
          return newOperands;
        };
        auto* ifTrueCall =
          makeDirectCall(getOperands(), select->ifTrue, flatTable, curr);
        auto* ifFalseCall =
          makeDirectCall(getOperands(), select->ifFalse, flatTable, curr);

        // Create the if to pick the calls, and emit the final block.
        auto* iff = builder.makeIf(select->condition, ifTrueCall, ifFalseCall);
        blockContents.push_back(iff);
        replaceCurrent(builder.makeBlock(blockContents));

        // By adding locals we must make type adjustments at the end.
        changedTypes = true;
      }
    }
  }

  void doWalkFunction(Function* func) {
    WalkerPass<PostWalker<FunctionDirectizer>>::doWalkFunction(func);
    if (changedTypes) {
      ReFinalize().walkFunctionInModule(func, getModule());
      TypeUpdating::handleNonDefaultableLocals(func, *getModule());
    }
  }

private:
  const std::unordered_map<Name, TableUtils::FlatTable>& tables;

  bool changedTypes = false;

  // Create a direct call for a given list of operands, an expression which is
  // known to contain a constant indicating the table offset, and the relevant
  // table. If we can see that the call will trap, instead return an
  // unreachable.
  Expression* makeDirectCall(const std::vector<Expression*>& operands,
                             Expression* c,
                             const TableUtils::FlatTable& flatTable,
                             CallIndirect* original) {
    Index index = c->cast<Const>()->value.geti32();

    // If the index is invalid, or the type is wrong, we can
    // emit an unreachable here, since in Binaryen it is ok to
    // reorder/replace traps when optimizing (but never to
    // remove them, at least not by default).
    if (index >= flatTable.names.size()) {
      return replaceWithUnreachable(operands);
    }
    auto name = flatTable.names[index];
    if (!name.is()) {
      return replaceWithUnreachable(operands);
    }
    auto* func = getModule()->getFunction(name);
    if (original->sig != func->getSig()) {
      return replaceWithUnreachable(operands);
    }

    // Everything looks good!
    return Builder(*getModule())
      .makeCall(name, operands, original->type, original->isReturn);
  }

  Expression* replaceWithUnreachable(const std::vector<Expression*>& operands) {
    // Emitting an unreachable means we must update parent types.
    changedTypes = true;

    Builder builder(*getModule());
    std::vector<Expression*> newOperands;
    for (auto* operand : operands) {
      newOperands.push_back(builder.makeDrop(operand));
    }
    return builder.makeSequence(builder.makeBlock(newOperands),
                                builder.makeUnreachable());
  }

  void optimizeITable(CallIndirect* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }
    // In TrapsNeverHappen mode, we can optimize this pattern:
    //
    //  (call_indirect $table (type $type)
    //   ..params..
    //   (struct.get $vtable $vtable.field
    //    (ref.cast $vtable
    //     (array.get $itable
    //      ..object..
    //      (i32.const K)))))
    //
    // An itable is basically a bundle of sub-vtables. We load one of those sub-
    // vtables by index K, then cast it to a particular vtable type $vtable,
    // then read a field $vtable.field from that vtable struct and then call it
    // on $table.
    //    
    // Even if we assume nothing about the input object, we know this:
    //  - We read from some itable at index K
    //  - What we find there is cast successfully to $vtable
    //  - We read $vtable.field from that
    //
    // And we can verify that
    //  - $vtable and $itable have immutable fields (for $vtable we can see that on the
    //    type, for $itable which is an array the wasm type system doesn't yet
    //    allow that (but it should, given array.init), so we need to verify it)
    //
    // If we are lucky, that narrows things down enough to infer a constant
    // index being passed to the call_indirect. Specifically, we can do this:
    //  - Find all $itable creations
    //  - Filter to ones where index K exists and holds an item of type $vtable (or sub).
    //  - For all those $vtable instances, see if at field $vtable.field we have
    //    the same constant value all the time.
    //
    // (Even without TNH we could do something here that just leaves a trap
    // around if the cast fails, then does a direct call.)
    auto* outerGet = curr->target->dynCast<StructGet>();
    if (!outerGet) {
      return;
    }
    auto* cast = outerGet->ref->dynCast<RefCast>();
    if (!cast) {
      return;
    }
    auto* innerGet = cast->ref->dynCast<ArrayGet>();
    if (!innerGet) {
      return;
    }
    if (!innerGet->index->is<Const>()) {
      return;
    }

std::cout << "waka1\n";
    // Looks like our pattern. Let's see what we can infer.
    auto vtableType = cast->getIntendedType();
//std::cout << "waka2\n";
    Index itableIndex = innerGet->index->cast<Const>()->value.geti32();
//std::cout << "waka3\n";
    auto vtableIndex = outerGet->index;
    auto itableType = innerGet->ref->type.getHeapType();
    auto& wasm = *getModule();
    std::unordered_set<Literal> possibleValues; // Small?
    for (auto& global : wasm.globals) {
//std::cout << "  shaka1\n";
      // Check it is an itable with the right type.
      if (global->imported() || !global->init->type.isRef() || !HeapType::isSubType(global->init->type.getHeapType(), itableType)) {
        continue;
      }
//std::cout << "  shaka2\n";
      // Check it is an array.init with enough items for our constant index.
      auto* init = global->init->dynCast<ArrayInit>();
      if (!init || init->values.size() <= itableIndex) {
        continue;
      }
//std::cout << "  shaka3\n";
      // Check the vtable is of the right type.
      auto* vtableNew = init->values[itableIndex]->dynCast<StructNew>();
      if (!vtableNew || !HeapType::isSubType(vtableNew->type.getHeapType(), vtableType)) {
        continue;
      }
      if (vtableNew->isWithDefault()) continue;
      auto* vtableItem = vtableNew->operands[vtableIndex];
      if (!Properties::isSingleConstantExpression(vtableItem)) {
        continue;
      }
      auto value = Properties::getLiteral(vtableItem);
      possibleValues.insert(value);
      if (possibleValues.size() > 1) {
        break;
      }
    }
std::cout << "  shaka4 " << (possibleValues.size() <= 1) << "\n"; // this finds nothing :(
  }
};

struct Directize : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // Find which tables are valid to optimize on. They must not be imported nor
    // exported (so the outside cannot modify them), and must have no sets in
    // any part of the module.

    // First, find which tables have sets.
    using TablesWithSet = std::unordered_set<Name>;

    ModuleUtils::ParallelFunctionAnalysis<TablesWithSet> analysis(
      *module, [&](Function* func, TablesWithSet& tablesWithSet) {
        if (func->imported()) {
          return;
        }
        for (auto* set : FindAll<TableSet>(func->body).list) {
          tablesWithSet.insert(set->table);
        }
      });

    TablesWithSet tablesWithSet;
    for (auto& kv : analysis.map) {
      for (auto name : kv.second) {
        tablesWithSet.insert(name);
      }
    }

    std::unordered_map<Name, TableUtils::FlatTable> validTables;

    for (auto& table : module->tables) {
      if (table->imported()) {
        continue;
      }

      if (tablesWithSet.count(table->name)) {
        continue;
      }

      bool canOptimizeCallIndirect = true;
      for (auto& ex : module->exports) {
        if (ex->kind == ExternalKind::Table && ex->value == table->name) {
          canOptimizeCallIndirect = false;
          break;
        }
      }
      if (!canOptimizeCallIndirect) {
        continue;
      }

      // All conditions are valid, this is optimizable.
      TableUtils::FlatTable flatTable(*module, *table);
      if (flatTable.valid) {
        validTables.emplace(table->name, flatTable);
      }
    }

    // Without typed function references, all we can do is optimize table
    // accesses, so if we can't do that, stop.
    if (validTables.empty() && !module->features.hasTypedFunctionReferences()) {
      return;
    }
    // The table exists and is constant, so this is possible.
    FunctionDirectizer(validTables).run(runner, module);
  }
};

} // anonymous namespace

Pass* createDirectizePass() { return new Directize(); }

} // namespace wasm
