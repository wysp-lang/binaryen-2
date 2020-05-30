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
// This pass is more effective on flat IR (--flatten) since when it
// instruments say a return, there will be no code run in the return's
// value.
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
      // We can be branched out of, so instrument the exit
      replaceCurrent(addPostLogging(curr))
    }
  }

  void visitIf(If* curr) {
    curr->ifTrue = addPreLogging(curr->ifTrue);
    if (curr->ifFalse) {
      curr->ifFalse = addPreLogging(curr->ifFalse);
    }
  }

  void visitLoop(Loop* curr) { curr->body = addPreLogging(curr->body); }

  void visitReturn(Return* curr) { replaceCurrent(addPreLogging(curr)); }

  void visitFunction(Function* curr) {
    if (curr->imported()) {
      return;
    }
    if (auto* block = curr->body->dynCast<Block>()) {
      if (!block->list.empty()) {
        block->list.back() = addPreLogging(block->list.back());
      }
    }
    curr->body = addPreLogging(curr->body);
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
    Builder builder(*getModule());
    return builder.makeSequence(makeLogCall(), curr);
  }

  Expression* addPostLogging(Expression* curr) {
    Builder builder(*getModule());
    return builder.makeSequence(curr, makeLogCall());
  }

  Expression* makeLogCall() {
    static Index id = 0;
    return
      builder.makeCall(
        LOGGER, {
         builder.makeConst(Literal(int32_t(id++)))}, Type::none);
  }
};

Pass* createLogExecutionPass() { return new LogExecution(); }

} // namespace wasm
