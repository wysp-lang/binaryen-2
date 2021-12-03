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
//       work as they compare equal + we track effects). part of value numbering?
//

#include <algorithm>
#include <memory>

#include "cfg/cfg-traversal.h"
#include "cfg/domtree.h"
#include "cfg/dominates.h"
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

  HashedShallowExpression(Expression* expr, size_t digest)
    : expr(expr), digest(digest) {}

  HashedShallowExpression(const HashedShallowExpression& other)
    : expr(other.expr), digest(other.digest) {}
};

struct HSEHasher {
  size_t operator()(const HashedShallowExpression hashed) const {
    return hashed.digest;
  }
};

// A full equality check for HashedShallowExpressions. The hash is used as a speedup,
// but if it matches we still verify the contents are identical.
struct HSEComparer {
  bool operator()(const HashedShallowExpression a, const HashedShallowExpression b) const {
    if (a.digest != b.digest) {
      return false;
    }
    return ExpressionAnalyzer::shallowEqual(a.expr, b.expr);
  }
};

// Maps hashed shallow expressions to the list of expressions that match. That is, all
// expressions that are equivalent (same hash, and also compare equal) will
// be in a vector for the corresponding entry in this map.
using HashedShallowExprs = std::unordered_map<HashedShallowExpression,
                                       SmallVector<Expression*, 1>,
                                       HSEHasher,
                                       HSEComparer>;

// Additional information for each basic block.
struct CSEBasicBlockInfo {
  // A list of all expressions in the block.
  std::vector<Expression*> list;
};

} // anonymous namespace

struct CSE : public WalkerPass<CFGWalker<CSE, UnifiedExpressionVisitor<CSE>, CSEBasicBlockInfo>> {
  bool isFunctionParallel() override { return true; }

  // FIXME DWARF updating does not handle local changes yet.
  bool invalidatesDWARF() override { return true; }

  Pass* create() override { return new CSE(); }

  // Information that helps us find copies.
  struct CopyInfo {
    // If not -1 then this is the index of a copy of the expression that
    // appears before.
    // TODO: This could be a set of copies, which would let us optimize a few
    //       more cases. Right now we just store the last copy seen here.
    Index copyOf = -1;

    // The complete size of the expression, that is, including nested children.
    // This in combination with copyOf lets us compute copies in parent
    // expressions using info from their children, which avoids quadratic
    // repeated work.
    Index fullSize = -1;
  };

  struct ExprInfo {
    // The original expression at this location. It may be replaced later as we
    // optimize.
    Expression* original;

    // The pointer to the expression, for use if we need to replace the expr.
    Expression** currp;

    CopyInfo copyInfo;

    // Whether this is the first of a set of repeated expressions, and we have
    // modified this one to store its value in a tee.
    bool replacedWithTee = false;

    ExprInfo(Expression* original, Expression** currp) : original(original), currp(currp) {}
  };

  // Info for each (reachable) expression in the function, in post-order.
  std::vector<Expression*> exprInfos;

  void visitExpression(Expression* curr) {
    if (currBasicBlock) {
      // Note each (reachable) expression and the exprInfo for it to it.
      currBasicBlock->contents.list.push_back(curr);
      currBasicBlock->contents.exprInfos.push_back(CSEBasicBlockInfo::ExprInfo(curr, getCurrentPointer()));
    }
  }

  void doWalkFunction(Function* func) {
    // First scan the code to find all the expressions and basic blocks. This
    // fills in |blocks| and starts to fill in |exprInfos|.
    WalkerPass<CFGWalker<CSE, UnifiedExpressionVisitor<CSE>>>::doWalkFunction(func);

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
    std::unordered_map<Expression*, Index> originalIndexes;

    // If we never find any relevant copies to optimize then we will give up
    // early later.
    bool foundRelevantCopy = false;

    for (Index i = 0; i < exprInfos.size(); i++) {
      auto& exprInfo = exprInfos[i];
      auto& copyInfo = exprInfo.copyInfo;
      auto* original = exprInfo.original;
      originalIndexes[original] = i;
      auto iter = seen.find(original);
      if (iter != seen.end()) {
        // We have seen this before. Note it is a copy of the last of the
        // previous copies.
        auto& previous = iter->second;
        assert(!previous.empty());
        copyInfo.copyOf = originalIndexes[previous.back()];
        previous.push_back(original);
      } else {
        // We've never seen this before. Add it.
        seen[original].push_back(original);
      }

      // Pop any children and see if we are part of a copy that
      // includes them.
      auto numChildren = ChildIterator(exprInfo.original).getNumChildren();
      Index totalSize = 1;
      for (Index child = 0; child < numChildren; child++) {
        auto childInfo = stack.back();
        stack.pop_back();

        // For us to be a copy of something, we need to have found a shallow
        // copy of ourselves, and also for all of our children to appear in the
        // right positions as children of that previous appearance. Once
        // anything is not perfectly aligned, we have failed to find a copy.
        if (copyInfo.copyOf != -1) {
          // The child's location is our own plus a shift of the
          // size we've seen so far. That is, the first child is right before
          // us in the vector, and the one before it is at an additiona offset
          // of the size of the last child, and so forth.
          auto childPosition = i - totalSize;

          // Check if this child has a copy, and that copy is perfectly aligned
          // with the parent that we found ourselves to be a shallow copy of.
          if (childInfo.copyOf == Index(-1) ||
              childInfo.copyOf != copyInfo.copyOf - totalSize) {
            copyInfo.copyOf = -1;
          }
        }

        // Regardless of copyOf status, keep accumulating the sizes of the
        // children. TODO: this is not really necessary since without a copy we
        // never look at the size, but that is a little subtle
        totalSize += childInfo.totalSize;
      }

      if (copyInfo.copyOf != Index(-1) && isRelevant(original)) {
        foundRelevantCopy = true;
      }

      stack.push_back(copyInfo);
    }

    if (!foundRelevantCopy) {
      return;
    }

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

    for (int i = int(exprInfos.size()) - 1; i >= 0; i--) {
      auto& exprInfo = exprInfos[i];
      auto& copyInfo = exprInfo.copyInfo;
      auto* original = exprInfo.original;

      if (copyInfo.copyOf == Index(-1)) {
        continue;
      }

      auto* copySource = exprInfos[copyInfo.copyOf].original;

      // There is a copy. See if the first dominates the second, as if not then
      // we can just skip.
      if (!dominationChecker.dominates(copySource, original)) {
        continue;
      }

      // It dominates. Check for effects getting in the way.
      // Note that we compute effects here in a way that is potentially
      // quadratic (as we'll check child effects later potentially). But it is
      // necessary to check like this
      // TODO: do an effects phase earlier? Also front to back? But the problem is
      //       that a parent might not have effects while a child does, if the
      //       child breaks to the parent, e.g. - so we do need quadratic work
      //       here in general. but likely that is ok as it is rare to have
      //       copies?
      EffectAnalyzer effects(runner->options, *module, original);

      // Side effects prevent us from removing a copy. We also cannot optimize away something that is intrinsically
      // nondeterministic: even if it has no side effects, if it may return a
      // different result each time, then we cannot optimize away repeats.
      if (effects.hasSideEffects() ||
          Properties::isGenerative(curr, module->features)) {
        continue;
      }

      // Everything looks good so far. Finally, check for effects along the way,
      // to verify that the first value will not be any different at the
      // second appearance.
      //
      // While checking for effects we must skip the pattern itself, including
      // children, as dominationChecker is not aware of nesting (it just looks
      // shallowly).
      std::unordered_set<Expression*> ignoreEffectsOf;
      // |i| must be at least as large as the size of the expression that is
      // based at |i| and includes its children.
      assert(i > copyInfo.totalSize);
      for (Index j = i - copyInfo.totalSize + 1; j <= i; j++) {
        ignoreEffectsOf.insert(exprInfos[j].original);
      }
      if (!dominationChecker.dominatesWithoutInterference(
        copySource,
        original,
        effects,
        ignoreEffectsOf,
        *module,
        runner->options)) {
        continue;
      }

      // Everything looks good! We can optimize here.
    }








    // Fix up any nondefaultable locals that we've added.
    TypeUpdating::handleNonDefaultableLocals(func, *getModule());
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
