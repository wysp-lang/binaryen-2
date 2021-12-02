/*
 * Copyright 2016 WebAssembly Community Group participants
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

#ifndef cfg_dominates_h
#define cfg_dominates_h

#include "cfg/domtree.h"
#include "cfg/locations.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm::cfg {

// A tool to check whether expressions dominate each other. At creation time
// this constructs a dominator tree and a mapping of expressions to their
// basic blocks, and then calls to dominates(x, y) use those data structures to
// give an answer to whether x dominates y.
template<typename BasicBlock> struct DominationChecker {
  DomTree<BasicBlock> domTree;
  BlockLocations<BasicBlock> blockLocations;

  DominationChecker(const std::vector<std::unique_ptr<BasicBlock>>& blocks)
    : domTree(blocks), blockLocations(blocks) {}

  // Returns whether x dominates y, that is, whether every code path from the
  // entry to y must pass through x.
  //
  // TODO: Add memoization here?
  bool dominates(Expression* x, Expression* y) {
    if (x == y) {
      // x dominates itself.
      return true;
    }

    const auto xLocation = blockLocations.locations[x];
    const auto yLocation = blockLocations.locations[y];

    // Start seeking from the original location of y, go back through y's
    // dominators and look for x there. Iff x dominates a chain of blocks up to
    // y then it dominates y.
    auto* seek = y;
    Index seekBlockIndex = yLocation.blockIndex;
    while (1) {
      if (xLocation.blockIndex > seekBlockIndex) {
        // x's block is after seek's block in reverse postorder. That means that
        // x cannot possibly dominate that block.
        return false;
      }

      if (xLocation.blockIndex == seekBlockIndex) {
        // We arrived at the same block, so we must now look inside it.
        if (seekBlockIndex == yLocation.blockIndex) {
          // This is the original block that y is in. Compare y's position in
          // the block to x;
          return xLocation.positionIndex <= yLocation.positionIndex;
        } else {
          // We seeked to a dominator of y's block, which means y is not in x's
          // block, and so x dominates y.
          return true;
        }
      }

      // Otherwise, keep looking back through the domtree.
      seekBlockIndex = domTree.iDoms[seekBlockIndex];
      if (seekBlockIndex == DomTree<BasicBlock>::nonsense) {
        // There is no dominator of the previous block, which means we are in
        // unreachable code, and so x does not dominate y (but that isn't really
        // very interesting given the unreachability; other opts should have
        // simplified things first).
        //
        // Note that this can't be the entry (which is only block without a
        // dominator that is not unreachable), as we checked earlier whether
        // we had gone past x's block, which we must do to reach the entry
        // (unless x's block is the entry, but we've handled that as well
        // earlier).
        return false;
      }
    }
  }
};

} // namespace wasm::cfg

#endif // cfg_dominates_h
