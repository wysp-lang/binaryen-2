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
// If called with
//
//   --pass-arg=directize-initial-contents-immutable
//
// then the initial tables' contents are assumed to be immutable. That is, if
// a table looks like [a, b, c] in the wasm, and we see a call to index 1, we
// will assume it must call b. It is possible that the table is appended to, but
// in this mode we assume the initial contents are not overwritten. This is the
// case for output from LLVM, for example.
//

#include <unordered_map>

#include "call-utils.h"
#include "ir/drop.h"
#include "ir/table-utils.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

namespace {

struct TableInfo {
  // Whether the table may be modifed at runtime, either because it is imported
  // or exported, or table.set operations exist for it in the code.
  bool mayBeModified = false;

  // Whether we can assume that the initial contents are immutable. See the
  // toplevel comment.
  bool initialContentsImmutable = false;

  std::unique_ptr<TableUtils::FlatTable> flatTable;

  bool canOptimize() const {
    // We can optimize if:
    //  * Either the table can't be modified at all, or it can be modified but
    //    the initial contents are immutable (so we can optimize them).
    //  * The table is flat.
    return (!mayBeModified || initialContentsImmutable) && flatTable->valid;
  }
};

struct TableInfoMap : public std::unordered_map<Name, TableInfo> {
  // We can optimize something if at least one table allows it.
  bool canOptimize() const {
    for (auto& [_, info] : *this) {
      if (info.canOptimize()) {
        return true;
      }
    }
    return false;
  }
};

struct TableCallOptimizer : public WalkerPass<PostWalker<TableCallOptimizer>> {
  bool isFunctionParallel() override { return true; }

  std::unique_ptr<Pass> create() override {
    return std::make_unique<TableCallOptimizer>(tables);
  }

  TableCallOptimizer(const TableInfoMap& tables) : tables(tables) {}

  void visitCallIndirect(CallIndirect* curr) {
    auto& table = tables.at(curr->table);
    if (!table.canOptimize()) {
      return;
    }
    // If the target is constant, we can emit a direct call.
    if (curr->target->is<Const>()) {
      std::vector<Expression*> operands(curr->operands.begin(),
                                        curr->operands.end());
      makeDirectCall(operands, curr->target, table, curr);
      return;
    }

    // Emit direct calls for things like a select over constants.
    if (auto* calls = CallUtils::convertToDirectCalls(
          curr,
          [&](Expression* target) {
            return getTargetInfo(target, table, curr);
          },
          *getFunction(),
          *getModule())) {
      replaceCurrent(calls);
      // Note that types may have changed, as the utility here can add locals
      // which require fixups if they are non-nullable, for example.
      changedTypes = true;
      return;
    }
  }

  void doWalkFunction(Function* func) {
    WalkerPass<PostWalker<TableCallOptimizer>>::doWalkFunction(func);
    if (changedTypes) {
      ReFinalize().walkFunctionInModule(func, getModule());
    }
  }

private:
  const TableInfoMap& tables;

  bool changedTypes = false;

  // Given an expression that we will use as the target of an indirect call,
  // analyze it and return one of the results of CallUtils::IndirectCallInfo,
  // that is, whether we know a direct call target, or we know it will trap, or
  // if we know nothing.
  CallUtils::IndirectCallInfo getTargetInfo(Expression* target,
                                            const TableInfo& table,
                                            CallIndirect* original) {
    auto* c = target->dynCast<Const>();
    if (!c) {
      return CallUtils::Unknown{};
    }

    Index index = c->value.geti32();

    // Check if index is invalid, or the type is wrong.
    auto& flatTable = *table.flatTable;
    if (index >= flatTable.names.size()) {
      // The index is out of bounds for the initial table's content. This may
      // trap, but it may also not trap if the table is modified later (if a
      // function is appended to it).
      if (!table.mayBeModified) {
        return CallUtils::Trap{};
      } else {
        // The table may be modified, so it might be appended to. We should only
        // get here in the case that the initial contents are immutable, as
        // otherwise we have nothing to optimize at all.
        assert(table.initialContentsImmutable);
        return CallUtils::Unknown{};
      }
    }
    auto name = flatTable.names[index];
    if (!name.is()) {
      return CallUtils::Trap{};
    }
    auto* func = getModule()->getFunction(name);
    if (original->heapType != func->type) {
      return CallUtils::Trap{};
    }
    return CallUtils::Known{name};
  }

  // Create a direct call for a given list of operands, an expression which is
  // known to contain a constant indicating the table offset, and the relevant
  // table, if we can. If we can see that the call will trap, instead replace
  // with an unreachable.
  void makeDirectCall(const std::vector<Expression*>& operands,
                      Expression* c,
                      const TableInfo& table,
                      CallIndirect* original) {
    auto info = getTargetInfo(c, table, original);
    if (std::get_if<CallUtils::Unknown>(&info)) {
      // We don't know anything here.
      return;
    }
    // If the index is invalid, or the type is wrong, we can skip the call and
    // emit an unreachable here (with dropped children as needed), since in
    // Binaryen it is ok to reorder/replace traps when optimizing (but never to
    // remove them, at least not by default).
    if (std::get_if<CallUtils::Trap>(&info)) {
      replaceCurrent(
        getDroppedChildrenAndAppend(original,
                                    *getModule(),
                                    getPassOptions(),
                                    Builder(*getModule()).makeUnreachable(),
                                    DropMode::IgnoreParentEffects));
      changedTypes = true;
      return;
    }

    // Everything looks good!
    auto name = std::get<CallUtils::Known>(info).target;
    replaceCurrent(
      Builder(*getModule())
        .makeCall(name, operands, original->type, original->isReturn));
  }
};

// In some situations we can infer which indirect call targets are possible,
// using the type or other information.
//
// To do so, we must have a closed world or else more targets are possible that
// we do not see statically.
struct PossibleCallOptimizer
  : public WalkerPass<PostWalker<PossibleCallOptimizer>> {
  bool isFunctionParallel() override { return true; }

  std::unique_ptr<Pass> create() override {
    return std::make_unique<PossibleCallOptimizer>();
  }

  // A map of function types to the functions that can be called. If a type is
  // not in this map then we have not yet computed that type. We build this
  // lazily to avoid unnecessary computation.
  //
  // TODO: Perhaps computing this once ahead of time could be faster than on-
  //       demand in each thread.
  std::unordered_map<HeapType, std::vector<Name>> typeTargets;

  template<typename T>
  void optimizeCall(T* curr, std::vector<Name>& targets) {
    // TODO: further filter using out arguments - if we see a cast will trap,
    //       the target is impossible in TNH

    Builder builder(*getModule());

    if (targets.empty()) {
      // Nothing can be called, so this will trap; we don't need the call.
      replaceCurrent(
        getDroppedChildrenAndAppend(curr,
                                    *getModule(),
                                    getPassOptions(),
                                    builder.makeUnreachable(),
                                    DropMode::IgnoreParentEffects));
      refinalize = true;
      return;
    }

    if (targets.size() == 1) {
      // We can optimize to a direct call.
      replaceCurrent(builder.makeCall(
        targets[0], curr->operands, curr->type, curr->isReturn));
    }

    // We would like to optimize the case of targets.size() == 2 as well, to an
    // if between two calls. However, function references cannot be compared, so
    // we don't have a way to check which of the two functions to call, even
    // though we know it must be one of the two.
    // TODO: Perhaps one thing we can do here is have an option to convert
    //       call_refs into call_indirects with separate tables. Then the table
    //       here could be of size targets.size(), and turn into an if when
    //       size() is 2, etc.
  }

  void visitCallRef(CallRef* curr) {
    auto type = curr->target->type;
    if (!type.isRef()) {
      return;
    }
    auto heapType = type.getHeapType();

    auto iter = typeTargets.find(heapType);
    if (iter == typeTargets.end()) {
      iter =
        typeTargets.emplace(heapType, findPossibleFunctions(heapType)).first;
    }
    auto& targets = iter->second;

    optimizeCall(curr, targets);
  }

  // TODO: call_indirect too

  // Given a function type, find all possible targets of that type, filtering
  // out ones we can prove are impossible.
  std::vector<Name> findPossibleFunctions(HeapType type) {
    auto trapsNeverHappen = getPassOptions().trapsNeverHappen;

    std::vector<Name> ret;
    for (auto& func : getModule()->functions) {
      // Filter out functions with an incompatible type.
      if (!HeapType::isSubType(func->type, type)) {
        continue;
      }

      // If the function body definitely traps then it can be assumed not to be
      // called in traps-never-happen mode. (Note that checking for the entire
      // body being unreachable is enough, as Vacuum will optimize into that
      // form.)
      if (trapsNeverHappen && !func->imported() &&
          func->body->is<Unreachable>()) {
        continue;
      }

      ret.push_back(func->name);
    }

    return ret;
  }

  void doWalkFunction(Function* func) {
    // All optimizations in this class depend on closed world.
    assert(getPassOptions().closedWorld);

    WalkerPass<PostWalker<PossibleCallOptimizer>>::doWalkFunction(func);

    if (refinalize) {
      ReFinalize().walkFunctionInModule(func, getModule());
    }
  }

private:
  bool refinalize = false;
};

struct Directize : public Pass {
  void run(Module* module) override {
    // Build table info first.
    TableInfoMap tables;
    buildTableInfo(module, tables);

    optimizeTableCalls(module, tables);
    optimizePossibleCalls(module, tables);
  }

  // Optimize CallIndirects using information about tables, that is, matching
  // table indexes to the contents in the table.
  void buildTableInfo(Module* module, TableInfoMap& tables) {
    if (module->tables.empty()) {
      return;
    }

    // TODO: consider a per-table option here
    auto initialContentsImmutable =
      getPassOptions().hasArgument("directize-initial-contents-immutable");

    // Set up the initial info.
    for (auto& table : module->tables) {
      tables[table->name].initialContentsImmutable = initialContentsImmutable;
      tables[table->name].flatTable =
        std::make_unique<TableUtils::FlatTable>(*module, *table);
    }

    // Next, look at the imports and exports.

    for (auto& table : module->tables) {
      if (table->imported()) {
        tables[table->name].mayBeModified = true;
      }
    }

    for (auto& ex : module->exports) {
      if (ex->kind == ExternalKind::Table) {
        tables[ex->value].mayBeModified = true;
      }
    }

    // This may already be enough information to know that we can't optimize
    // anything. If so, skip scanning all the module contents.
    if (!tables.canOptimize()) {
      return;
    }

    // Find which tables have sets.

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

    for (auto& [_, names] : analysis.map) {
      for (auto name : names) {
        tables[name].mayBeModified = true;
      }
    }
  }

  void optimizeTableCalls(Module* module, const TableInfoMap& tables) {
    if (!tables.canOptimize()) {
      return;
    }

    TableCallOptimizer(tables).run(getPassRunner(), module);
  }

  // Optimize using an analysis of which call targets are possible using the
  // type and other information.
  void optimizePossibleCalls(Module* module, const TableInfoMap& tables) {
    // We can only optimize call_indirect and call_ref, so exit early if those
    // instructions are not possible.
    if (module->tables.empty() && !module->features.hasGC()) {
      return;
    }

    // We must be in a closed world to see all the possible call targets.
    if (!getPassOptions().closedWorld) {
      return;
    }

    PossibleCallOptimizer().run(getPassRunner(), module);
  }
};

} // anonymous namespace

Pass* createDirectizePass() { return new Directize(); }

} // namespace wasm
