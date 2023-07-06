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
//

#include "ir/possible-contents.h"
#include "pass.h"
#include "wasm-type.h"
#include "wasm.h"

namespace wasm {

namespace {

struct Unsubtyping : public Pass {
  // Only modifies types.
  bool requiresNonNullableLocalFixups() override { return false; }

  void run(Module* module) override {
    if (!module->features.hasGC()) {
      return;
    }

    // Build the graph of all places where content can flow. Using this graph we
    // can optimize according to the simple rule of modifying the parent of a
    // type to be the least-refined type that the type can be written to. That
    // is, if we have A :> B :> C and
    //
    //  B* bloc1 = new B(); // a B is written to a place of type B
    //  A* aloc1 = new C(); // A C is written to a place of type A
    //
    // If we have no other uses of these types, then B does not need any
    // (non-basic) supertype, as it is not written to any location of type A.
    // C, on the other hand, is written to a location of type A, so's supertype
    // can be A and not C, giving us A :> C, skipping B in the middle. If we
    // also had
    //
    //  B* bloc2 = new C(); // A C is written to a place of type B
    //
    // when we'd need to have A :> B :> C, that is, we need to keep using the
    // entire original subtyping tree, including the connection between B and A,
    // simply for C's benefit, since C is written to locations of both types A
    // and B.
    PossibleContentsGraph graph(*module);

    
  }
};

} // anonymous namespace

Pass* createUnsubtypingPass() { return new Unsubtyping(); }

} // namespace wasm
