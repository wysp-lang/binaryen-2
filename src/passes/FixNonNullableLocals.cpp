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
// Fix up non-nullable locals: Passes can make many changes that break the
// validation of non-nullable locals, which validate according to the "1a"
// rule of each get needing to be structurally dominated by a set - sets allow
// gets until the end of the set's block. Any time a pass adds a block, that
// can break:
//
//  (local.set $x ..)
//  (local.get $x)
//
// =>
//
//  (block
//    ..new code..
//    (local.set $x ..)
//  )
//  (local.get $x)
//
// This example is the common case of adding new code at a location by
// wrapping it in a block and appending or prepending the old code. But now
// the set does not structurally dominate the get. To avoid each pass needing
// to handle this, do it after every function-parallel pass, which is the
// vast majority of passes. The few non-function-parallel passes need to add
// this handling themselves if they require it (not doing it by default
// avoids iterating on all functions in each such pass, which may be
// wasteful).
//

#include "ir/type-updating.h"
#include "pass.h"
#include "wasm.h"

namespace wasm {

struct FixNonNullableLocals
  : public WalkerPass<PostWalker<FixNonNullableLocals>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new FixNonNullableLocals(); }

  void doWalkFunction(Function* func) {
    TypeUpdating::handleNonDefaultableLocals(func, *getModule());
  }
};

Pass* createFixNonNullableLocalsPass() { return new FixNonNullableLocals(); }

} // namespace wasm
