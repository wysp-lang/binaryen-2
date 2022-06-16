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

#include <pass.h>
#include <wasm-builder.h>
#include <wasm-validator.h>
#include <wasm.h>

namespace wasm {

struct TrapsNeverHappen : public WalkerPass<ExpressionStackWalker<TrapsNeverHappen>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new TrapsNeverHappen; }

  void visitRefAs(RefAs* curr) {
    if (expressionStack.size() < 2) {
      return;
    }

    assert(expressionStack.back() == curr);
    auto* parent = expressionStack[expressionStack.size() - 2];

    // Check if the wasm still validates without us. If so, we are not needed.
    replaceCurrent(curr->value);
    if (!WasmValidator().validate(parent, getFunction(), *getModule())) {
      // It did not validate; restore the old state.
      replaceCurrent(curr);
      // TODO returned value, and value flowing out, must match the function
      //      results. block results too...
std::cout << "fail " << getFunction()->name << '\n';
    }
  }

  // TODO: RefCast
};

Pass* createTrapsNeverHappenPass() { return new TrapsNeverHappen(); }

} // namespace wasm
