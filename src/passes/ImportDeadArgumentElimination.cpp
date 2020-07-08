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
#include <sstream>

#include "ir/module-utils.h"
#include "pass.h"
#include "support/json.h"
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
    // Next, find import arguments that are constant.
    struct CalledImportInfo {
      // Track the values going to each param, using a value for each one, which
      // indicates the single constant value seen, or if we've seen more than
      // one, InvalidValue, indicating we can't optimize there.
      std::vector<Literal> params;
      // All the calls to this import.
      std::vector<Call*> calls;
    };
    std::map<Name, CalledImportInfo> calledImportInfoMap;
    for (auto& pair : scan.map) {
      for (auto* call : pair.second.importCalls) {
        auto* called = module->getFunction(call->target);
        assert(called->imported());
        if (called->sig.params.size() == 0) {
          continue;
        }
        auto& info = calledImportInfoMap[call->target];
        info.calls.push_back(call);
        bool first = false;
        auto num = call->operands.size();
        auto& params = info.params;
        if (params.empty()) {
          // This is the first time we see this imported function.
          first = true;
          params.resize(num);
        }
        for (Index i = 0; i < num; i++) {
          auto* operand = call->operands[i];
          if (!operand->is<Const>()) {
            // A nonconstant value means we must give up on optimizing this.
            params[i] = InvalidValue;
            continue;
          }
          // Otherwise, if there is an existing value it must match.
          auto literal = operand->cast<Const>()->value;
          if (first) {
            params[i] = literal;
          } else if (literal != params[i]) {
            params[i] = InvalidValue;
          }
        }
      }
    }
    // We now know which arguments are removeable.
    json::Value output;
    output.setArray();
    for (auto& pair : calledImportInfoMap) {
      auto& info = pair.second;
      // Note that this does not attempt to handle the case of an import that is
      // never called, as the params here will be empty. (Other optimization
      // passes would remove such an import anyhow.)
      auto& params = info.params;
      auto num = params.size();
      // We may remove more than one parameter from the same function, so track
      // how much we have already shrunk. We start from 0, so later offsets need
      // to be aware of removed earlier ones.
      Index removed = 0;
      for (Index i = 0; i < num; i++) {
        if (params[i] != InvalidValue) {
          // Report the argument is not needed, so that the other side can
          // handle that. We report the import module and base, the index of the
          // parameter, and the value.
          auto* called = module->getFunction(pair.first);
          json::Value entry;
          entry.setArray(4);
          entry[0] = &json::Value(called->module.str);
          entry[1] = &json::Value(called->base.str);
          entry[2] = &json::Value(i);
          std::stringstream ss;
          ss << params[i];
          entry[3] = &json::Value(ss.str().c_str());
          output.push_back(&entry);
          // Remove the argument from the imported function's signature and from
          // all calls to it.
          auto vector = called->sig.params.expand();
          Index adjustedIndex = i - removed;
          vector.erase(vector.begin() + adjustedIndex);
          called->sig.params = Type(vector);
          for (auto* call : info.calls) {
            call->operands.erase(call->operands.begin() + adjustedIndex);
          }
          removed++;
        }
      }
    }
    output.stringify(std::cout, /* pretty= */ true);
  }
};

} // anonymous namespace

Pass* createIDAEPass() { return new IDAE(); }

Pass* createIDAEOptimizingPass() { return new IDAE(); }

} // namespace wasm
