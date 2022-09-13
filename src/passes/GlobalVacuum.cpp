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
//
// function bar() {
//   foo();
// }
//
// foo does nothing anyhow, so we can remove the call. Note that in such trivial
// cases inlining does what we want anyhow, so this pass is useful for larger
// functions. It also helps when using the call.without.effects intrinsic.
//

#include "ir/drop.h"
#include "ir/intrinsics.h"
#include "ir/module-utils.h"
#include "pass.h"
#include "support/unique_deferring_queue.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

struct GlobalVacuum : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // First, find the relevant things in each function.
    struct Info
      : public ModuleUtils::CallGraphPropertyAnalysis<Info>::FunctionInfo {

      // Whether the function has effects. Note that we only care about non-
      // removable ones since we'll be removing calls to here, if we can do so).
      enum {
        // We know this has effects.
        Yes,
        // We know this has no effects.
        No,
        // We don't know yet. This is the status a function is in while we
        // figure out if the things it calls have effects.
        Pending
      } effects = Pending;
    };

    // Perform any temporary allocations we may need below in a temporary
    // module, so they all get GC'd automatically at the end.
    Module tempModule;

    ModuleUtils::CallGraphPropertyAnalysis<Info> analyzer(
      *module, [&](Function* func, Info& info) {
        if (func->imported()) {
          // Assume an import has effects, unless it is call.without.effects.
          info.effects = Intrinsics(*module).isCallWithoutEffects(func)
                           ? Info::No
                           : Info::Yes;
          return;
        }

        // Gather the effects.
        EffectAnalyzer effects(runner->options, *module, func->body);

        // We can ignore branching out of the function body - this can only be
        // a return, and that is only noticeable in the function, not outside.
        effects.branchesOut = false;

        // We want to ignore calls, since we'll be computing them explicitly
        // below, transitively. If calls exist, and exceptions are enabled, then
        // calls were marked as throwing, and as a result we'll need to
        // recompute effect to remove that.
        if (effects.calls && passRunner->options.hasExceptions()) {
          // Copy the function and make the calls lack effects, then recompute
          // effects on that.
          auto* funcCopy = ModuleUtils::copyFunction(func, tempModule);

          struct CallEffectRemover : public WalkerPass<PostWalker<CallEffectRemover>> {
            void visitCall(Call* curr) {
              
            }
          } callEffectRemover;

          callEffectRemover.setPassOptions(runner->options);
          callEffectRemover.walkFunctionInModule(funcCopy, &tempModule);
        }
        assert(!effects.calls);

        // Ignore local writes - when the function exits, those become
        // unnoticeable anyhow.
        effects.localsWritten.clear();

        // We have effects either if the effect handler found any, or if we have
        // a loop (which may hang forever, which is not something we can
        // remove).
        if (effects.hasUnremovableSideEffects() ||
            !FindAll<Loop>(func->body).list.empty()) {
          info.effects = Info::Yes;
        }
      });

    auto& map = analyzer.map;

    // Anything with a non-direct call is assumed to have effects. Anything with
    // no effects in itself, and that has no calls at all, definitely has no
    // effects.
    for (auto& [func, info] : map) {
      if (info.hasNonDirectCall) {
        info.effects = Info::Yes;
      } else if (info.effects == Info::Pending && info.callsTo.empty()) {
        info.effects = Info::No;
      }
    }

    // Propagate the property of having no effects to callers. The basic idea is
    // that if a function has no effects in its body, and everything it calls
    // also has no effects, then it has no effects. Start by queuing all items
    // that have no effects. This queue will contain items that we just learned
    // have no effects.
    UniqueDeferredQueue<Function*> queue;
    for (auto& [func, info] : map) {
      if (info.effects == Info::No) {
        queue.push(func);
      }
    }

    // Process the queue. As we work, we remove items from |callsTo|. That is,
    // once we see that foo() has no effects, we process all callers and remove
    // their call to foo() (since we can ignore it). Once such a caller has no
    // calls left, we can mark it has having no effects itself (if it has no
    // effects in its body).
    while (!queue.empty()) {
      auto* func = queue.pop();
      assert(map[func].effects == Info::No);
      for (auto* caller : map[func].calledBy) {
        auto& callerInfo = map[caller];
        auto& callerCallsTo = callerInfo.callsTo;
        assert(callerCallsTo.count(func));
        if (callerInfo.effects == Info::Pending) {
          // This is pending, so there is a chance this can be found to have no
          // effects. Remove the call.
          callerCallsTo.erase(func);
          if (callerCallsTo.empty()) {
            // No calls left; we've proven this has no effects.
            callerInfo.effects = Info::No;
            queue.push(caller);
          }
        }
      }
    }

    // We now know which functions have effects we cannot remove. Calls to
    // functions without such effects can be removed.
    struct Optimize : public WalkerPass<PostWalker<Optimize>> {
      bool isFunctionParallel() override { return true; }

      Pass* create() override { return new Optimize(map); }

      std::map<Function*, Info>& map;

      Optimize(std::map<Function*, Info>& map) : map(map) {}

      void visitDrop(Drop* curr) {
        if (curr->type != Type::none) {
          // Ignore unreachable code.
          return;
        }
        if (auto* call = curr->value->dynCast<Call>()) {
          replaceIfCallHasNoEffects(call);
        }
      }

      void visitCall(Call* curr) {
        // The case of a call that returns a value is handled in visitDrop. Here
        // we just look at none-returning ones.
        if (curr->type == Type::none) {
          replaceIfCallHasNoEffects(curr);
        }
      }

      // Replace the current expression in the walk if a given call has no side
      // effects.
      void replaceIfCallHasNoEffects(Call* call) {
        auto* target = getModule()->getFunction(call->target);
        if (map[target].effects == Info::No) {
          auto* nop = Builder(*getModule()).makeNop();
          replaceCurrent(alwaysGetDroppedChildrenAndAppend(
            call, *getModule(), getPassOptions(), nop));
        }
      }

      void visitFunction(Function* curr) {
        // If entire function body has no effects, and it does not return a
        // value, then nop it out. This optimizes the case where the only
        // effects are either calls, that turn out to not actually have effects
        // in our analysis, or local sets, which do not matter after the
        // function exits.
        if (map[curr].effects == Info::No) {
          curr->body = Builder(*getModule()).makeNop();
        }
      }
    };
    Optimize(map).run(runner, module);
  }
};

Pass* createGlobalVacuumPass() { return new GlobalVacuum(); }

} // namespace wasm
