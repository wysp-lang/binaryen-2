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
// CSE
//
// Full-function analysis.
//
// Can handle even nested control flow.
//
// TODO: use value numbering to get GVN.
// TODO: use a LocalGraph to match gets with tees (gets with gets should already
//       work as they compare equal + we track effects). part of value
//       numbering?
//

#include <algorithm>
#include <memory>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include "cfg/cfg-traversal.h"
#include "cfg/dominates.h"
#include "cfg/domtree.h"
#include "ir/cost.h"
#include "ir/effects.h"
#include "ir/iteration.h"
#include "ir/properties.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

namespace {

// An expression with a cached hash value. Hashing is done shallowly, that is,
// ignoring children.
struct HashedShallowExpression {
  Expression* expr;
  size_t digest;

  HashedShallowExpression(Expression* expr) : expr(expr) {
    digest = ExpressionAnalyzer::shallowHash(expr);
  }

  HashedShallowExpression(const HashedShallowExpression& other)
    : expr(other.expr), digest(other.digest) {}
};

struct HSEHasher {
  size_t operator()(const HashedShallowExpression hashed) const {
    return hashed.digest;
  }
};

// A full equality check for HashedShallowExpressions. The hash is used as a
// speedup, but if it matches we still verify the contents are identical.
struct HSEComparer {
  bool operator()(const HashedShallowExpression a,
                  const HashedShallowExpression b) const {
    if (a.digest != b.digest) {
      return false;
    }
    return ExpressionAnalyzer::shallowEqual(a.expr, b.expr);
  }
};

// Maps hashed shallow expressions to the list of expressions that match. That
// is, all expressions that are equivalent (same hash, and also compare equal)
// will be in a vector for the corresponding entry in this map.
using HashedShallowExprs = std::unordered_map<HashedShallowExpression,
                                              SmallVector<Index, 1>,
                                              HSEHasher,
                                              HSEComparer>;

// Additional information for each basic block.
struct CSEBasicBlockInfo {
  // A list of all expressions in the block.
  std::vector<Expression*> list;
};

static const Index ImpossibleIndex = -1;

// Information that helps us find copies.
struct CopyInfo {
  // If not -1 then this is the index of a copy of the expression that
  // appears before.
  // TODO: This could be a set of copies, which would let us optimize a few
  //       more cases. Right now we just store the last copy seen here.
  SmallVector<Index, 1> copyOf;

  // The complete size of the expression, that is, including nested children.
  // This in combination with copyOf lets us compute copies in parent
  // expressions using info from their children, which avoids quadratic
  // repeated work.
  Index fullSize = ImpossibleIndex;
};

struct ExprInfo {
  // The original expression at this location. It may be replaced later as we
  // optimize.
  Expression* original;

  // The pointer to the expression, for use if we need to replace the expr.
  Expression** currp;

  CopyInfo copyInfo;

  // If this has a tee, these are the indexes of locations that read from it.
  std::vector<Index> teeReads;

  ExprInfo(Expression* original, Expression** currp)
    : original(original), currp(currp) {}
};

struct Linearize
  : public PostWalker<Linearize, UnifiedExpressionVisitor<Linearize>> {
  // Info for each (reachable) expression in the function, in post-order.
  std::vector<ExprInfo> exprInfos;

  void visitExpression(Expression* curr) {
    exprInfos.push_back(ExprInfo(curr, getCurrentPointer()));
  }
};

} // anonymous namespace

struct CSE
  : public WalkerPass<
      CFGWalker<CSE, UnifiedExpressionVisitor<CSE>, CSEBasicBlockInfo>> {
  bool isFunctionParallel() override { return true; }

  // FIXME DWARF updating does not handle local changes yet.
  bool invalidatesDWARF() override { return true; }

  Pass* create() override { return new CSE(); }

  void visitExpression(Expression* curr) {
    // std::cout << "visit\n" << *curr << '\n';
    if (currBasicBlock) {
      currBasicBlock->contents.list.push_back(curr);
    }
  }

  void doWalkFunction(Function* func) {
std::cout << "func " << func->name << '\n';
    // First scan the code to find all the expressions and basic blocks. This
    // fills in |blocks| and starts to fill in |exprInfos|.
    WalkerPass<
      CFGWalker<CSE, UnifiedExpressionVisitor<CSE>, CSEBasicBlockInfo>>::
      doWalkFunction(func);
std::cout << "  a\n";

    Linearize linearize;
    linearize.walk(func->body);
    auto exprInfos = std::move(linearize.exprInfos);
std::cout << "  b\n";

    // Do another pass to find repeated expressions. We are looking for complete
    // expressions, including children, that recur, and so it is efficient to do
    // this using a stack: the stack will contain the children of the current
    // expression and whether we've found previous copies of them, and from that
    // we can see if the parent also repeats.
    // (This could be done during the first pass of ::doWalkFunction, perhaps,
    // for efficiency? TODO but it might be less clear)
    std::vector<CopyInfo> stack;

    // Track the shallow expressions that we've seen so far.
    HashedShallowExprs seen;

    // The indexes at which expressions reside. TODO this could be part of
    // the hashed expression perhaps, avoiding an extra map. That is, we could
    // store indexes while we hash.
    std::unordered_map<Expression*, Index> originalIndexes; // is this teh slow?

    // If we never find any relevant copies to optimize then we will give up
    // early later.
    bool foundRelevantCopy = false;

    auto* module = getModule();
    auto& passOptions = getPassOptions();

    for (Index i = 0; i < exprInfos.size(); i++) {
      auto& exprInfo = exprInfos[i];
      auto& copyInfo = exprInfo.copyInfo;
      auto* original = exprInfo.original;
      // std::cout << "first loop " << i << '\n' << *original << '\n';
      originalIndexes[original] = i;
      auto iter = seen.find(original);

EffectAnalyzer effects(passOptions, *module);
effects.visit(original);
// We can ignore traps here, as we replace a repeating expression with a
// single appearance of it, a store to a local, and gets in the other
// locations, and so if the expression traps then the first appearance -
// that we keep around - would trap, and the others are never reached
// anyhow. (The other checks we perform here, including invalidation and
// determinism, will ensure that either all of the appearances trap, or
// none of them.)
effects.trap = false;
if (effects.hasSideEffects() || // TODO: nonremovable?
    Properties::isGenerative(original, module->features)) {
  // std::cout << "  effectey\n";
  stack.push_back(copyInfo); // empty, no copies, which will cause fails
  continue;
}

// TODO: copyInfo on |stack| could be just a reference? do not copy that array!

      if (iter != seen.end()) {
        // std::cout << "  seen, append\n";
        // We have seen this before. Note it is a copy of the last of the
        // previous copies.
        auto& previous = iter->second;
        assert(!previous.empty());
        copyInfo.copyOf = previous;
        previous.push_back(i); // limit on total size? so a func with 1,000,000 local.gets doesn't crowd too much.
      } else {
        // std::cout << "  novel\n";
        // We've never seen this before. Add it.
        seen[original].push_back(i);
      }

      // Pop any children and see if we are part of a copy that
      // includes them.
      auto numChildren = ChildIterator(exprInfo.original).getNumChildren();
      copyInfo.fullSize = 1;
      for (Index child = 0; child < numChildren; child++) { // limit on numChildren? be reasonable. also, can effects just make us give up early?
        assert(!stack.empty());
        auto childInfo = stack.back();
        // std::cout << "  child " << child << " of full size " <<
        // childInfo.fullSize <<"\n";
        stack.pop_back();

        // For us to be a copy of something, we need to have found a shallow
        // copy of ourselves, and also for all of our children to appear in the
        // right positions as children of that previous appearance. Once
        // anything is not perfectly aligned, we have failed to find a copy.
        // We basically convert copyInfo.copyOf from a list of shallow copy
        // locations to a list of full copy locations, which is a subset of that
        // (and maybe empty). FIXME refactor
        SmallVector<Index, 1> filteredCopiesOf;
        for (auto copy : copyInfo.copyOf) {
          // std::cout << "    childCopy1, copy=" << originalIndexes[copy] << "
          // , copyInfo.copyOf=" << copyInfo.copyOf.size() << " ,
          // copyInfo.fullSize=" << copyInfo.fullSize << "\n";
          // The child's location is our own plus a shift of the
          // size we've seen so far. That is, the first child is right before
          // us in the vector, and the one before it is at an additiona offset
          // of the size of the last child, and so forth.
          // Check if this child has a copy, and that copy is perfectly aligned
          // with the parent that we found ourselves to be a shallow copy of.
          for (auto childCopy : childInfo.copyOf) {
            // std::cout << "    childCopy2 " << originalIndexes[childCopy] << "
            // vs  " << (originalIndexes[copy] - copyInfo.fullSize) << "\n";
            if (childCopy ==
                copy - copyInfo.fullSize) {
              // std::cout << "    childCopy3\n";
              filteredCopiesOf.push_back(copy);
              break;
            }
          }
        }
        copyInfo.copyOf = std::move(filteredCopiesOf);

        // Regardless of copyOf status, keep accumulating the sizes of the
        // children. TODO: this is not really necessary since without a copy we
        // never look at the size, but that is a little subtle
        copyInfo.fullSize += childInfo.fullSize;
      }
      // std::cout << *original << " has fullSize " << copyInfo.fullSize <<
      // '\n';
      if (!copyInfo.copyOf.empty() && isRelevant(original)) {
        foundRelevantCopy = true;
      }

      stack.push_back(copyInfo);
    }

    if (!foundRelevantCopy) {
      return;
    }
    // std::cout << "phase 2\n";

std::cout << "  c\n";

    // We have filled in |exprInfos| with copy information, and we've found at
    // least one relevant copy. We can now apply those copies. We start at the
    // end and go back, so that we always apply the largest possible match,
    // that is, if we have
    //
    //   A  B  C  ...  A  B  C
    //
    // where C is the parent of A and B, then we want to see C first so that we
    // can replace all of it.
    //
    // To see which copies can actually be optimized, we need to see that the
    // first dominates the second.
    cfg::DominationChecker<BasicBlock> dominationChecker(basicBlocks);
std::cout << "  d\n";

    Builder builder(*module);

    bool optimized = false;

    for (int i = int(exprInfos.size()) - 1; i >= 0; i--) {
      auto& currInfo = exprInfos[i]; // rename to info or currInfo?
      auto& copyInfo = currInfo.copyInfo;
      auto* curr = currInfo.original;
      // std::cout << "phae 2 i " << i << " : " << getExpressionName(curr) <<
      // '\n';

      if (copyInfo.copyOf.empty()) {
        // std::cout << "  no copies\n";
        continue;
      }

      if (!isRelevant(curr)) {
        // std::cout << "  irrelevant\n";
        // This has a copy, but it is not relevant to optimize. (We mus still
        // track such things as copies as their parents may be relevant.)
        continue;
      }

      // The index of the first child, or if there is no such child, the same
      // value as i. This is the beginning of the range of indexes that includes
      // the expression and all its children.
      auto firstChild = i - int(copyInfo.fullSize) + 1;
      assert(firstChild >= 0 && firstChild <= i);

      // Mark us as a copy of the last possible source. That is the closest to
      // us, and the most likely to not run into interference along the way that
      // would prevent optimization.
      Expression* source = nullptr;
      for (int j = int(copyInfo.copyOf.size()) - 1; j >= 0; j--) {
        auto possibleSourceIndex = copyInfo.copyOf[j];
        auto* possibleSource = exprInfos[possibleSourceIndex].original;
        if (dominationChecker.dominates(possibleSource, curr)) {
          source = possibleSource;
          break;
        }
      }
      if (!source) {
        // std::cout << "  no dom\n";
        continue;
      }

      // It dominates. Check for effects getting in the way.
      // Note that we compute effects here in a way that is potentially
      // quadratic (as we'll check child effects later potentially). But it is
      // necessary to check like this
      // TODO: do an effects phase earlier? Also front to back? But the problem
      // is
      //       that a parent might not have effects while a child does, if the
      //       child breaks to the parent, e.g. - so we do need quadratic work
      //       here in general. but likely that is ok as it is rare to have
      //       copies?
      EffectAnalyzer effects(passOptions, *module, curr);

      // We can ignore traps here, as we replace a repeating expression with a
      // single appearance of it, a store to a local, and gets in the other
      // locations, and so if the expression traps then the first appearance -
      // that we keep around - would trap, and the others are never reached
      // anyhow. (The other checks we perform here, including invalidation and
      // determinism, will ensure that either all of the appearances trap, or
      // none of them.)
      auto oldTrap = effects.trap;
      effects.trap = false;

      // Side effects prevent us from removing a copy. We also cannot optimize
      // away something that is intrinsically nondeterministic: even if it has
      // no side effects, if it may return a different result each time, then we
      // cannot optimize away repeats.
      if (effects.hasSideEffects()) { // TODO: nonremovable?
        // std::cout << "  effectey\n";
        continue;
      }

      // Return to the original trapping information for any further effect
      // checks. TODO does this matter?
      effects.trap = oldTrap;

      // Everything looks good so far. Finally, check for effects along the way,
      // to verify that the first value will not be any different at the
      // second appearance.
      //
      // While checking for effects we must skip the pattern itself, including
      // children, as dominationChecker is not aware of nesting (it just looks
      // shallowly).
      std::unordered_set<Expression*> ignoreEffectsOf;
      for (int j = firstChild; j <= i; j++) {
        ignoreEffectsOf.insert(exprInfos[j].original);
      }
      if (!dominationChecker.dominatesWithoutInterference(
            source, curr, effects, ignoreEffectsOf, *module, passOptions)) {
        // std::cout << "  pathey\n";
        continue;
      }

      // Everything looks good! We can optimize here.
      optimized = true;
      auto sourceIndex = originalIndexes[source];
      auto& sourceInfo = exprInfos[sourceIndex];
      // std::cout << "optimize!!! " << i << " to " << sourceIndex << "\n";
      // This is the first time we add a tee on the source in order to use its
      // value later (that is, we've not found another copy of |source|
      // earlier), so add a new local and add a tee.
      //
      // If |curr| already has a tee placed on it - which can happen if it is
      // the source for some appearance we've already optimized - then we've
      // already allocated a local, and can reuse that. Note that this is safe
      // to do as the previous copy is dominated by |curr|, and |curr| is
      // dominated by |source|, so |source| dominates the previous copy. When
      // doing so we remove the tee from here, so that we basically are just
      // moving the single tee backwards.
      auto moveTeeReads = [&](Index from, Index to) {
        for (auto index : exprInfos[from].teeReads) {
          exprInfos[to].teeReads.push_back(index);
        }
        exprInfos[from].teeReads.clear();
      };
      moveTeeReads(i, sourceIndex);
      // std::cout << "TEE\n";
      // We'll add the actual tee at the end. That avoids effect confusion,
      // as tees add side effects.
      sourceInfo.teeReads.push_back(i);

      // We handled the case of the entire expression already having a tee on
      // it, which was trivial. We must do something similar for our children:
      // if we are replacing the entire expression with a local.get then any tee
      // will be removed. But we found a previous copy of this entire
      // expression, which means that the child also appears earlier, and so we
      // can move the tee earlier.
      for (int j = firstChild; j < i; j++) {
        if (!exprInfos[j].teeReads.empty()) {
          // Remove the tee from this location and move it to the corresponding
          // location in the source - that is, at the exact same offset from the
          // source as we are from |i|.
          int offsetInExpression = i - j;
          assert(offsetInExpression > 0);
          int k = sourceIndex - offsetInExpression;
          assert(k >= 0);
          // std::cout << "move tee from " << j << " to " << k << '\n';
          moveTeeReads(j, k);
        }
      }

      // Skip over all of our children before the next loop iteration, since we
      // have optimized the entire expression, including them, into a single
      // local.get.
      int childrenSize = copyInfo.fullSize - 1;
      // std::cout << "chldrensize " << childrenSize << '\n';
      // < and not <= becauase not only the children exist, but also the copy
      // before us that we optimize to.
      assert(childrenSize < i);
      i -= childrenSize;
    }

    if (!optimized) {
      return;
    }

std::cout << "  e\n";

    Index z = 0;
    for (auto& info : exprInfos) {
      if (!info.teeReads.empty()) {
        auto* original = info.original;
        auto type = original->type;
        auto tempLocal = builder.addVar(getFunction(), type);
        *info.currp = builder.makeLocalTee(tempLocal, original, type);
        for (auto read : info.teeReads) {
          *exprInfos[read].currp = builder.makeLocalGet(tempLocal, type);
        }
      }
      z++;
    }

std::cout << "  f\n";

    // Fix up any nondefaultable locals that we've added.
    TypeUpdating::handleNonDefaultableLocals(func, *getModule());
std::cout << "  g\n";
  }

private:
  // Only some values are relevant to be optimized.
  bool isRelevant(Expression* curr) {
    // * Ignore anything that is not a concrete type, as we are looking for
    //   computed values to reuse, and so none and unreachable are irrelevant.
    // * Ignore local.get and set, as those are the things we optimize to.
    // * Ignore constants so that we don't undo the effects of constant
    //   propagation.
    // * Ignore things we cannot put in a local, as then we can't do this
    //   optimization at all.
    //
    // More things matter here, like having side effects or not, but computing
    // them is not cheap, so leave them for later, after we know if there
    // actually are any requests for reuse of this value (which is rare).
    if (!curr->type.isConcrete() || curr->is<LocalGet>() ||
        curr->is<LocalSet>() || Properties::isConstantExpression(curr) ||
        !TypeUpdating::canHandleAsLocal(curr->type)) {
      return false;
    }

    auto& options = getPassOptions();

    // If the size is at least 3, then if we have two of them we have 6,
    // and so adding one set+one get and removing one of the items itself
    // is not detrimental, and may be beneficial.
    // TODO: investigate size 2
    if (options.shrinkLevel > 0 && Measurer::measure(curr) >= 3) {
      return true;
    }

    // If we focus on speed, any reduction in cost is beneficial, as the
    // cost of a get is essentially free.
    if (options.shrinkLevel == 0 && CostAnalyzer(curr).cost > 0) {
      return true;
    }

    return false;
  }
};

Pass* createCSEPass() { return new CSE(); }

} // namespace wasm

#pragma GCC diagnostic pop
