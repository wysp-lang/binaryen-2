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
// Converts GC instructions with RTTs to their sttaic forms, by just dropping
// the RTT instructions.
//

#include <ir/iteration.h>
#include <pass.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

struct DeRTT : public WalkerPass<PostWalker<DeRTT>> {
  void visitRefTest(RefTest* curr) {
    handleCast(curr);
  }
  void visitRefCast(RefCast* curr) {
    handleCast(curr);
  }
  void visitBrOn(BrOn* curr) {
    if (curr->op == BrOnCast || curr->op == BrOnCastFail) {
      handleCast(curr);
    }
  }

  void visitStructNew(StructNew* curr) {
    handleNew(curr);
  }
  void visitArrayNew(ArrayNew* curr) {
    handleNew(curr);
  }
  void visitArrayInit(ArrayInit* curr) {
    handleNew(curr);
  }

  // Cast instructions update their intendedType field.
  template<typename T>
  void handleCast(T* curr) {
    Builder builder(*getModule());

    if (curr->rtt->type == Type::unreachable) {
      handleUnreachable(curr);
      return;
    }

    // In the simple/reachable case, just remove the RTT and update the type.
    curr->intendedType = curr->rtt->type.getHeapType();
    curr->rtt = nullptr;
  }

  // New/creation instructions update their type field.
  template<typename T>
  void handleNew(T* curr) {
    Builder builder(*getModule());

    if (curr->type == Type::unreachable) {
      handleUnreachable(curr);
      return;
    }

    curr->type = Type(curr->rtt->type.getHeapType(), NonNullable);
    curr->rtt = nullptr;
  }

  // If the RTT is unreachable, emit the children without the RTT.
  template<typename T>
  void handleUnreachable(T* curr) {
    Builder builder(*getModule());
    std::vector<Expression*> items;
    for (auto* child : ChildIterator(curr)) {
      if (child != curr->rtt) {
        items.push_back(child);
      }
      // Add an unreachable to handle the case where the RTT was the only
      // unreachable thing.
      items.push_back(builder.makeUnreachable());
    }
    replaceCurrent(builder.makeBlock(items));
  }
};

Pass* createDeRTTPass() { return new DeRTT(); }

} // namespace wasm
