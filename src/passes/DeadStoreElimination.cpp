/*
 * Copyright 2015 WebAssembly Community Group participants
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
// Finds stores that are trampled over by other stores anyhow, before they can
// be read.
//


#include <ir/local-graph.h>
#include <pass.h>
#include <wasm.h>

namespace wasm {

struct DeadStoreElimination
  : public WalkerPass<PostWalker<DeadStoreElimination>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new DeadStoreElimination; }

  void doWalkFunction(Function* curr) {
    UseDefAnalysisParams params;

    params.isUse = [](Expression* curr) { return curr->is<LocalGet>(); };

    params.isDef = [](Expression* curr) { return curr->is<LocalSet>(); };

    params.getLane = [](Expression* curr) {
               if (auto* get = curr->dynCast<LocalGet>()) {
                 return get->index;
               } else if (auto* set = curr->dynCast<LocalSet>()) {
                 return set->index;
               }
               WASM_UNREACHABLE("bad use-def expr");
             };

    params.numLanes = func->getNumLocals();

    params.noteUseDef =
             [&](Expression* use, Expression* def) {
               useDefs[use->cast<LocalGet>()].insert(def ? def->cast<LocalSet>()
                                                         : nullptr);
             };

    UseDefAnalysis analyzer;
    analyzer.analyze(curr, params);
  }
};

Pass* createDeadStoreEliminationPass() { return new DeadStoreElimination(); }

} // namespace wasm
