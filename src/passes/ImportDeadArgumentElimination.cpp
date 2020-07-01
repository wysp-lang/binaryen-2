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
// TODO: handle arguments not at the end, that is, if foo(x, y) has a constant
//       for x but not y, we will call foo(y).
//

#include <unordered_map>
#include <unordered_set>

#include "ir/module-utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

// Indicates an invalid value, something we cannot optimize.
static Literal invalidValue(none);

struct IDAE : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // First, find all calls to imports.
    struct Info {
      std::vector<Call*> importCalls;
    };
    ModuleUtils::ParallelFunctionAnalysis<Info> scan(*module, [module](Function* func, Info& info) {
      if (func->imported()) return;
      FindAll<Call> calls(func->body);
      for (auto* call : calls) {
        if (module->getFunction(call->target)->imported()) {
          info.importCalls.push_back(call);
        }
      }
    });
    // Next, find import arguments that are constant, for which we track the
    // values going to each called import.
    // A map of parameter index => literal constant value. If we have seen more
    // than one value, the value becomes invalidValue and no optimization is
    // possible there.
    using CalledImportInfo = std::vector<Literal> constantValues;
    std::unordered_map<Name, CalledImportInfo> calledImportInfoMap;
    for (auto& pair : scan.map) {
      for (auto* call : pair.second.importCalls) {
        auto* called = module->getFunction(call->target);
        assert(called->imported());
        if (called->sig.params.size() == 0) continue;
        auto& info = calledImportInfoMap[call->target];
        bool first = false;
        if (info.empty()) {
          // This is the first time we see this imported function.
          first = true;
          info.resize(call->operands.size());
        }
        for (auto* operand : call->operands) {
          if (!operand->isConst()) {
            // A nonconstant value means we must give up on optimizing this.
            info[index] = invalidValue;
            continue;
          }
          // Otherwise, if there is an existing value it must match.
          auto literal = operand->cast<Const>()->value;
          if (first) {
            info[index] = literal;
          } else if (literal != info[index]) {
            it->second = invalidValue;
          }
        }
      }
    }
  }
};

} // anonymous namespace

Pass* createIDAEPass() { return new IDAE(); }

Pass* createIDAEOptimizingPass() {
  auto* ret = new IDAE();
  ret->optimize = true;
  return ret;
}

} // namespace wasm
