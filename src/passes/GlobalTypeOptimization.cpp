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
// Optimize types at the global level, altering fields etc. on the set of heap
// types defined in the module.
//
//  * Immutability: If a field has no struct.set, it can become immutable.
//
// TODO: Specialize field types.
// TODO: Remove unused fields.
//

#include "ir/effects.h"
#include "ir/struct-utils.h"
#include "ir/subtypes.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "support/small_set.h"
#include "wasm-builder.h"
#include "wasm-type.h"
#include "wasm.h"

using namespace std;

namespace wasm {

namespace {

// Information about usage of a field.
struct FieldInfo {
  bool hasWrite = false;
  bool hasRead = false;

  void noteWrite() { hasWrite = true; }
  void noteRead() { hasRead = true; }

  bool combine(const FieldInfo& other) {
    bool changed = false;
    if (!hasWrite && other.hasWrite) {
      hasWrite = true;
      changed = true;
    }
    if (!hasRead && other.hasRead) {
      hasRead = true;
      changed = true;
    }
    return changed;
  }
};

struct FieldInfoScanner : public Scanner<FieldInfo, FieldInfoScanner> {
  Pass* create() override {
    return new FieldInfoScanner(functionNewInfos, functionSetInfos);
  }

  FieldInfoScanner(FunctionStructValuesMap<FieldInfo>& functionNewInfos,
                   FunctionStructValuesMap<FieldInfo>& functionSetGetInfos)
    : Scanner<FieldInfo, FieldInfoScanner>(functionNewInfos, functionSetGetInfos) {
  }

  void noteExpression(Expression* expr,
                      HeapType type,
                      Index index,
                      FieldInfo& info) {
    info.noteWrite();
  }

  void
  noteDefault(Type fieldType, HeapType type, Index index, FieldInfo& info) {
    info.noteWrite();
  }

  void noteCopy(HeapType type, Index index, FieldInfo& info) {
    info.noteWrite();
  }

  void visitStructGet(StructGet* curr) {
    if (curr->ref->type == Type::unreachable) {
      return;
    }

    functionSetInfos[getFunction()][curr->ref->type.getHeapType()][curr->index].noteRead();
  }
};

struct GlobalTypeOptimization : public Pass {
  StructValuesMap<FieldInfo> combinedSetGetInfos;

  // Maps types to a vector of booleans that indicate a particular property.
  // To avoid eager allocation of memory, the vectors are
  // only resized when we actually have a true to place in them (which is
  // rare).
  using HeapBoolVec = std::unordered_map<HeapType, std::vector<bool>>;

  // Track which fields can become immutable.
  HeapBoolVec canBecomeImmutable;

  // Track which fields can be removed. A removable field is one that is
  // "write-only", that is, we write to it, but never read anything back.
  HeapBoolVec canBeRemoved;

  void run(PassRunner* runner, Module* module) override {
    if (getTypeSystem() != TypeSystem::Nominal) {
      Fatal() << "GlobalTypeOptimization requires nominal typing";
    }

    // Find and analyze struct operations inside each function.
    FunctionStructValuesMap<FieldInfo> functionNewInfos(*module),
      functionSetGetInfos(*module);
    FieldInfoScanner scanner(functionNewInfos, functionSetGetInfos);
    scanner.run(runner, module);
    scanner.walkModuleCode(module);

    // Combine the data from the functions.
    functionSetGetInfos.combineInto(combinedSetGetInfos);
    // TODO: combine newInfos as well, once we have a need for that (we will
    //       when we do things like subtyping).

    // TODO: we need to propagate in both directions for no-read as well, update commentttt

    // Find which fields are immutable in all super- and sub-classes. To see
    // that, propagate sets in both directions. This is necessary because we
    // cannot have a supertype's field be immutable while a subtype's is not -
    // they must match for us to preserve subtyping.
    //
    // Note that we do not need to care about types here: If the fields were
    // mutable before, then they must have had identical types for them to be
    // subtypes (as wasm only allows the type to differ if the fields are
    // immutable). Note that by making more things immutable we therefore make
    // it possible to apply more specific subtypes in subtype fields.
    TypeHierarchyPropagator<FieldInfo> propagator(*module);
    propagator.propagateToSuperAndSubTypes(combinedSetGetInfos);

    for (auto type : propagator.subTypes.types) {
      if (!type.isStruct()) {
        continue;
      }

      auto& fields = type.getStruct().fields;
      for (Index i = 0; i < fields.size(); i++) {
        processImmutability(type, i, fields[i]);
        processRemovability(type, i, fields[i]);
      }
    }

    // Update the types in the entire module.
    updateTypes(*module);

    // If we found fields that can be removed, remove them from instructions
    // too.
    if (!canBeRemoved.empty()) {
      removeFieldsInInstructions(*module);
    }
  }

  void updateTypes(Module& wasm) {
    class TypeRewriter : public GlobalTypeRewriter {
      GlobalTypeOptimization& parent;

    public:
      TypeRewriter(Module& wasm, GlobalTypeOptimization& parent)
        : GlobalTypeRewriter(wasm), parent(parent) {}

      virtual void modifyStruct(HeapType oldStructType, Struct& struct_) {
        if (!parent.canBecomeImmutable.count(oldStructType)) {
          return;
        }

        auto& newFields = struct_.fields;
        auto& immutableVec = parent.canBecomeImmutable[oldStructType];
        for (Index i = 0; i < immutableVec.size(); i++) {
          if (immutableVec[i]) {
            newFields[i].mutable_ = Immutable;
          }
        }
      }
    };

    TypeRewriter(wasm, *this).update();
  }

  // After updating the types to remove certain fields, we must also remove
  // them from struct instructions.
  void removeFieldsInInstructions(Module& wasm) {
    struct FieldRemover
      : public WalkerPass<PostWalker<FieldRemover>> {
      bool isFunctionParallel() override { return true; }

      GlobalTypeOptimization& parent;

      FieldRemover(GlobalTypeOptimization& parent) : parent(parent) {}

      FieldRemover* create() override {
        return new FieldRemover(parent);
      }

      void visitStructNew(StructNew* curr) {
        if (curr->type == Type::unreachable) {
          return;
        }
        if (curr->isWithDefault()) {
          // Nothing to do, a default was written and will no longer be.
          return;
        }

        auto iter = parent.canBeRemoved.find(curr->type.getHeapType());
        if (iter == parent.canBeRemoved.end()) {
          return;
        }
        auto& vec = iter->second;

        auto& operands = curr->operands;
        auto numRelevantOperands = std::min(operands.size(), vec.size());
        for (Index i = 0; i < numRelevantOperands; i++) {
          if (vec[i]) {
            if (EffectAnalyzer(getPassOptions(), *getModule(), operands[i]).hasUnremovableSideEffects()) {
              Fatal() << "TODO: handle side effects in field removal (impossible in global locations?)"
            }
            operands[i] = nullptr;
          }
        }

        Index skip = 0;
        for (Index i = 0; i < operands.size(); i++) {
          if (operands[i]) {
            operands[i - skip] = operands[i];
          } else {
            skip++;
          }
        }
        operands.resize(operands.size() - skip);
      }

      void visitStructSet(StructSet* curr) {
        if (curr->ref->type == Type::unreachable) {
          return;
        }

        if (remove(curr->ref->type.getHeapType(), curr->index)) {
          Builder builder(*getModule());
          replaceCurrent(
            builder.makeSequence(
              builder.makeDrop(curr->ref),
              builder.makeDrop(curr->value)
            )
          );
        }
      }

      void visitStructGet(StructGet* curr) {
        if (curr->ref->type == Type::unreachable) {
          return;
        }

        if (remove(curr->ref->type.getHeapType(), curr->index)) {
          replaceCurrent(
            builder(*getModule()).makeDrop(curr->value)
          );
        }
      }

      bool remove(HeapType type, Index index) {
        auto iter = parent.canBeRemoved.find(curr->ref->type.getHeapType());
        if (iter == parent.canBeRemoved.end()) {
          return false;
        }
        auto& vec = iter->second;
        if (index >= vec.size()) {
          return false;
        }
        return vec[index];
      }
    };

    FieldRemover scanner(*this);
    scanner.run(runner, &wasm);
    scanner.walkModuleCode(&wasm);
  }

  void processImmutability(HeapType type, Index i, const Field& field) {
    if (field.mutable_ == Immutable) {
      // Already immutable; nothing to do.
      return;
    }

    if (combinedSetGetInfos[type][i].hasWrite) {
      // A set exists.
      return;
    }

    // No set exists. Mark it as something we can make immutable.
    auto& vec = canBecomeImmutable[type];
    vec.resize(i + 1);
    vec[i] = true;
  }

  void processRemovability(HeapType type, Index i, const Field& field) {
    if (combinedSetGetInfos[type][i].hasRead) {
      // A read exists.
      return;
    }

    // No read exists. Mark it as something we can remove.
    auto& vec = canBeRemoved[type];
    vec.resize(i + 1);
    vec[i] = true;
std::cout << "waka\n";
  }
};

} // anonymous namespace

Pass* createGlobalTypeOptimizationPass() {
  return new GlobalTypeOptimization();
}

} // namespace wasm
