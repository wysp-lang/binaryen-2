/*
 * Copyright 2022 WebAssembly Community Group participants
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
// Remove calls to code that has no effects. For example:
//
// function foo() {}
// function bar() {
//   foo();
// }
//
// foo does nothing anyhow, so we can remove the call. Note that in such trivial
// cases inlining does what we want anyhow, so this pass is useful for larger
// functions. It also helps when using the call.without.effects intrinsic.
//

#include "ir/intrinsics.h"
#include "ir/module-utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

struct GlobalVacuum : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // First, find the relevant things in each function.
    struct Info
      : public ModuleUtils::CallGraphPropertyAnalysis<Info>::FunctionInfo {

      // Whether the function has effects (we only care about non-removable ones
      // since we'll be removing the call, if we can do so).
      bool hasUnremovableSideEffects;
    };

    ModuleUtils::CallGraphPropertyAnalysis<Info> analyzer(
      *module, [&](Function* func, Info& info) {
        if (func->imported()) {
          // Assume an import has effects, unless it is call.without.effects.
          info.hasUnremovableSideEffects = !Intrinsics::isCallWithoutEffects(func);
          return;
        }

        // Gather the effects. Ignore calls - we'll compute them transitively
        // later.
        EffectAnalyzer effects(runner->options, *module, func->body);
        effects.calls = false;
        info.hasUnremovableSideEffects = effects.hasUnremovableSideEffects();

        // Note that we don't need to handle call.without.effects here in any
        // special way:
        // Find calls and handle them.
        struct CallFinder : public PostWalker<CallFinder> {
          CallFinder(Info& info) : info(info) {}

          visitCallIndirect(CallIndirect* call) {
            // Assume indirect calls can do anything. TODO optimize
            info.hasUnremovableSideEffects = true;
          }
          visitCallRef(CallRef* call) {
            // Assume indirect calls can do anything. TODO optimize
            info.hasUnremovableSideEffects = true;
          }
        };

        CallFinder callFinder(info);
        callFinder.walk(func->body);
      });

    // Propagate the property of having effects to callers. We ignore non-
    // direct calls since we handled them ourselves earlier.
    analyzer.propagateBack(
      [](const Info& info) { return info.hasUnremovableSideEffects; },
      [](const Info& info) { return true; },
      [](Info& info, Function* reason) { info.hasUnremovableSideEffects = true; },
      analyzer.IgnoreNonDirectCalls);

    // We now know which functions have effects we cannot remove. Calls to
    // functions without such effects can be removed.
    struct Optimize : public WalkerPass<PostWalker<Optimize>> {
      bool isFunctionParallel() override { return true; }

      Pass* create() override { return new Optimize(map); }

      std::map<Function*, Info>& map;

      Optimize(std::map<Function*, Info>& map)
        : map(map) {}

      void visitDrop(Drop* curr) {
        if (curr->type != Type::none) {
          // Ignore unreachable code.
          return;
        }
        if (auto* call = curr->value->dynCast<Call>()) {
          auto* target = getModule()->getFunction(call->target);
          if (map[target].hasUnremovableSideEffects) {
            return;
          }
                  // replace with droped childs
todo share codes
        }
      }

      void visitCall(Call* curr) {
        // The case of a call that returns a value is handled in visitDrop. Here
        // we just look at none-returning ones.
        if (curr->type != Type::none) {
          return;
        }
        auto* target = getModule()->getFunction(curr->target);
        if (map[target].hasUnremovableSideEffects) {
          return;
        }

        // replace with droped childs
      }
    };
    Optimize(analyzer.map).run(runner, module);
  }
};

Pass* createGlobalVacuumPass() { return new GlobalVacuum(); }

} // namespace wasm
