/*
 * Copyright 2020 WebAssembly Community Group participants
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
// Similar to DeadArgumentElimination, this looks for arguments that have
// constant values and so can be removed. The main pass does that inside wasm,
// while this one does it for imports. This pass *must* be run in conjunction
// with a transform on the outside as well, as we will remove the "dead"
// arguments, which only works if the outside applies their fixed value there.
// Concretely, if we call an import foo(x, y) always with a constant 42 in
// the second argument, then we will replace that import with foo(x) with no
// second argument at all. The proper value for the second argument is logged
// out so that the outside can handle it.
//
// We also handle arguments not at the end, that is, if foo(x, y) has a constant
// for x but not y, we will call foo(y).
//

#include <map>

#include "ir/module-utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

// Indicates an invalid value, something we cannot optimize.
static Literal InvalidValue(Type::none);

struct IDAE : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // First, find all calls to imports.
    struct Info {
      std::vector<Call*> importCalls;
    };
    ModuleUtils::ParallelFunctionAnalysis<Info> scan(
      *module, [module](Function* func, Info& info) {
        if (func->imported()) {
          return;
        }
        FindAll<Call> calls(func->body);
        for (auto* call : calls.list) {
          if (module->getFunction(call->target)->imported()) {
            info.importCalls.push_back(call);
          }
        }
      });
    // Next, find import arguments that are constant, for which we track the
    // values going to each called import, using a value for each parameter,
    // which indicates the single constant value seen, or if we've seen more
    // than one, InvalidValue, indicating we can't optimize there.
    using CalledImportInfo = std::vector<Literal>;
    std::map<Name, CalledImportInfo> calledImportInfoMap;
    for (auto& pair : scan.map) {
      for (auto* call : pair.second.importCalls) {
        auto* called = module->getFunction(call->target);
        assert(called->imported());
        if (called->sig.params.size() == 0) {
          continue;
        }
        auto& info = calledImportInfoMap[call->target];
        bool first = false;
        auto num = call->operands.size();
        if (info.empty()) {
          // This is the first time we see this imported function.
          first = true;
          info.resize(num);
        }
        for (Index i = 0; i < num; i++) {
          auto* operand = call->operands[i];
          if (!operand->is<Const>()) {
            // A nonconstant value means we must give up on optimizing this.
            info[i] = InvalidValue;
            continue;
          }
          // Otherwise, if there is an existing value it must match.
          auto literal = operand->cast<Const>()->value;
          if (first) {
            info[i] = literal;
          } else if (literal != info[i]) {
            info[i] = InvalidValue;
          }
        }
      }
    }
    // We now know which arguments are removeable.
    for (auto& pair : calledImportInfoMap) {
      auto& results = pair.second;
      // Note that this does not attempt to handle the case of an import that is
      // never called, as the results here will be empty. (Other optimization
      // passes would remove such an import anyhow.)
      auto num = results.size();
      for (Index i = 0; i < num; i++) {
        if (results[i] != InvalidValue) {
          // Report the argument is not needed, so that the other side can
          // handle that. We report the import module and base, the index of the
          // parameter, and the value.
          auto* func = module->getFunction(pair.first);
          std::cout << "[IDAE: remove (" << func->module << "," << func->base
                    << "," << i << "," << results[i] << ")]\n";
        }
      }
    }
  }
};

} // anonymous namespace

Pass* createIDAEPass() { return new IDAE(); }

Pass* createIDAEOptimizingPass() { return new IDAE(); }

} // namespace wasm
