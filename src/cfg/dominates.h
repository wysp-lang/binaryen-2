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
#include "ir/effects.h"
#include "support/unique_deferring_queue.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm::cfg {

// A tool to check whether expressions dominate each other. At creation time
// this constructs a dominator tree and a mapping of expressions to their
// basic blocks, and then calls to dominates(x, y) use those data structures to
// give an answer to whether x dominates y.
template<typename BasicBlock> struct DominationChecker {
  const std::vector<std::unique_ptr<BasicBlock>>& blocks;
  DomTree<BasicBlock> domTree;
  BlockLocations<BasicBlock> blockLocations;

  DominationChecker(const std::vector<std::unique_ptr<BasicBlock>>& blocks)
    : blocks(blocks), domTree(blocks), blockLocations(blocks) {}

  // Returns whether x dominates y, that is, whether every code path from the
  // entry to y must pass through x.
  //
  // TODO: Add memoization here?
  bool dominates(Expression* x, Expression* y) {
    if (x == y) {
      // x dominates itself.
      return true;
    }

    auto xIter = blockLocations.locations.find(x);
    if (xIter == blockLocations.locations.end()) {
      return false;
    }
    const auto xLocation = xIter->second;

    auto yIter = blockLocations.locations.find(y);
    if (yIter == blockLocations.locations.end()) {
      return false;
    }
    const auto yLocation = yIter->second;

    // Start seeking from the original location of y, go back through y's
    // dominators and look for x there. Iff x dominates a chain of blocks up to
    // y then it dominates y.
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

  // Checks whether x dominates y, and if it does so, that it also dominates it
  // without any effects interfering in the middle. That is, all paths from x
  // to y are free from effects that would invalidate the given effects. A set
  // of expressions to ignore the effects of is also provided.
  bool dominatesWithoutInterference(
    Expression* x,
    Expression* y,
    EffectAnalyzer& effects,
    const std::unordered_set<Expression*>& ignoreEffectsOf,
    Module& wasm,
    const PassOptions& passOptions) {
//std::cout << "     domwithoutInt1\n";
    if (x == y) {
      return true;
    }
//std::cout << "     domwithoutInt2\n";
    if (!dominates(x, y)) { // TODO: change API to assume it dominates? Avoid dupe work
      return false;
    }
//std::cout << "     domwithoutInt3\n";

    // x dominates y, so what we have left to check is for effects along the
    // way.
    const auto xLocation = blockLocations.locations[x];
    const auto yLocation = blockLocations.locations[y];

    // Define a helper function to check for effects in a range of positions
    // inside a block, which returns true if we found a problem. The range is in
    // [x, y) form, that is, y is not checked.
    // We allow positionEnd to be -1, which is interpreted as the end of the
    // list.
    auto hasInterference = [&](const std::vector<Expression*>& list,
                               Index positionStart,
                               Index positionEnd) {
      if (positionEnd == Index(-1)) {
        positionEnd = list.size();
      }
      for (Index i = positionStart; i < positionEnd; i++) {
        assert(i < list.size());
        auto* curr = list[i];
        if (ignoreEffectsOf.count(curr)) {
          continue;
        }
        EffectAnalyzer currEffects(passOptions, wasm);
        currEffects.visit(list[i]);
        if (currEffects.invalidates(effects)) {
          return true;
        }
      }
      return false;
      // TODO: add memoization?
    };

    // First, look for effects inside x's and y's blocks, after x and before y.
    // Often effects right next to x and y can save us looking any further.
    if (xLocation.blockIndex != yLocation.blockIndex) {
      if (hasInterference(blocks[xLocation.blockIndex]->contents.list,
                          xLocation.positionIndex + 1,
                          Index(-1)) ||
          hasInterference(blocks[yLocation.blockIndex]->contents.list,
                          0,
                          yLocation.positionIndex)) {
//std::cout << "     domwithoutInt4\n";
        return false;
      }
    } else {
      if (hasInterference(blocks[xLocation.blockIndex]->contents.list,
                          xLocation.positionIndex + 1,
                          yLocation.positionIndex)) {
//std::cout << "     domwithoutInt5\n";
        return false;
      }
      // We have no more blocks to scan, and have scanned the relevant parts
      // of the single block.
//std::cout << "     domwithoutInt6\n";
      return true;
    }

    auto* xBlock = blocks[xLocation.blockIndex].get();
    auto* yBlock = blocks[yLocation.blockIndex].get();

    // Look through the blocks between them, using a work queue of blocks to
    // scan. We ignore repeats here since we only need to ever scan a block
    // once.
    UniqueNonrepeatingDeferredQueue<BasicBlock*> work;
    for (auto* pred : yBlock->in) {
      work.push(pred);
    }
    while (!work.empty()) {
      auto* currBlock = work.pop();

      // As x dominates y, we know that if we keep going back through the
      // preds then eventually we will reach x, at which point we can stop
      // flowing.
      if (currBlock == xBlock) {
        continue;
      }

      // This is the first time we reach this block; scan it. (Note that we we
      // might reach y's block here, which we have partially scanned before; but
      // it is correct to scan all of that block now, as it is inside a loop and
      // therefore all the block is on a path from x to y.)
      if (hasInterference(currBlock->contents.list, 0, Index(-1))) {
//std::cout << "     domwithoutInt7\n";
        return false;
      }

      for (auto* pred : currBlock->in) {
        work.push(pred);
      }
    }

//std::cout << "     domwithoutInt8\n";
    // We saw before that x dominates y, and we found no interference along the
    // way, so we succeeded in showing that x dominates y without interference.
    return true;
  }
};

} // namespace wasm::cfg

#endif // cfg_dominates_h
