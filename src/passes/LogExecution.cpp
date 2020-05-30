/*
 * Copyright 2017 WebAssembly Community Group participants
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
// Instruments the build with code to log execution at each function
// entry, loop header, return, and other interesting control flow locations.
// This can be useful in debugging, to log out a trace, and diff it to another
// (running in another browser, to check for bugs, for example).
//
// The logging is performed by calling an ffi with an id for each
// call site. You need to provide that import on the JS side.
//

#include "asm_v_wasm.h"
#include "asmjs/shared-constants.h"
#include "shared-constants.h"
#include <pass.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

Name LOGGER("log_execution");

struct LogExecution : public WalkerPass<PostWalker<LogExecution>> {
  void visitBlock(Block* curr) {
    if (curr->name.is()) {
      // We can be branched out of, so instrument the exit.
      replaceCurrent(addPostLogging(curr));
    }
  }

  void visitIf(If* curr) {
    curr->ifTrue = addPreLogging(curr->ifTrue);
    if (curr->ifFalse) {
      curr->ifFalse = addPreLogging(curr->ifFalse);
    }
  }

  void visitLoop(Loop* curr) { curr->body = addPreLogging(curr->body); }

  void visitReturn(Return* curr) {
    if (!curr->value) {
      replaceCurrent(addPreLogging(curr));
    }
    // Add a local so we can log right before the return.
    Builder builder(*getModule());
    auto temp = Builder::addVar(getFunction(), curr->type);
    auto* value = curr->value;
    curr->value = builder.makeLocalGet(temp, curr->value);
    return builder.makeBlock({
      builder.makeLocalSet(temp, curr->value),
      makeLogCall(),
      curr
    });
  }

  void visitFunction(Function* curr) {
    if (curr->imported()) {
      return;
    }
    curr->body = addPostLogging(addPreLogging(curr->body));
  }

  void visitModule(Module* curr) {
    // Add the import
    auto import = new Function;
    import->name = LOGGER;
    import->module = ENV;
    import->base = LOGGER;
    import->sig = Signature(Type::i32, Type::none);
    curr->addFunction(import);
  }

private:
  Expression* addPreLogging(Expression* curr) {
    return Builder(*getModule()).makeSequence(makeLogCall(), curr);
  }

  Expression* addPostLogging(Expression* curr) {
    if (curr->type == unreachable) {
      // We'll never get to anything after it anyhow.
      return curr;
    }
    Builder builder(*getModule());
    if (curr->type == none) {
      return builder.makeSequence(curr, makeLogCall());
    }
    // Add a local to return the value properly.
    auto temp = Builder::addVar(getFunction(), curr->type);
    return builder.makeBlock({
      builder.makeLocalSet(temp, curr),
      makeLogCall(),
      builder.makeLocalGet(temp, curr->type)
    });
  }

  Expression* makeLogCall() {
    static Index id = 0;
    Builder builder(*getModule());
    return builder.makeCall(
      LOGGER, {builder.makeConst(Literal(int32_t(id++)))}, Type::none);
  }
};

Pass* createLogExecutionPass() { return new LogExecution(); }

} // namespace wasm
