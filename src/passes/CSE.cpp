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

  HashedShallowExpression(Expression* expr)
    : expr(expr) {
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
                                              SmallVector<Expression*, 1>,
                                              HSEHasher,
                                              HSEComparer>;

// Additional information for each basic block.
struct CSEBasicBlockInfo {
  // A list of all expressions in the block.
  std::vector<Expression*> list;
};

static const Index ImpossibleIndex = -1;

} // anonymous namespace

struct CSE
  : public WalkerPass<
      CFGWalker<CSE, UnifiedExpressionVisitor<CSE>, CSEBasicBlockInfo>> {
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
    SmallVector<Expression*, 1> copyOf;

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

    // Whether this is the first of a set of repeated expressions, and we have
    // modified this one to store its value in a tee.
    bool addedTee = false;

    ExprInfo(Expression* original, Expression** currp)
      : original(original), currp(currp) {}
  };

  // Info for each (reachable) expression in the function, in post-order.
  std::vector<ExprInfo> exprInfos;

  void visitExpression(Expression* curr) {
    if (currBasicBlock) {
      // Note each (reachable) expression and the exprInfo for it to it.
      currBasicBlock->contents.list.push_back(curr);
      exprInfos.push_back(
        ExprInfo(curr, getCurrentPointer()));
    }
  }

  void doWalkFunction(Function* func) {
std::cout << "func " << func->name << '\n';
    // First scan the code to find all the expressions and basic blocks. This
    // fills in |blocks| and starts to fill in |exprInfos|.
    WalkerPass<
      CFGWalker<CSE, UnifiedExpressionVisitor<CSE>, CSEBasicBlockInfo>>::doWalkFunction(
      func);

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
std::cout << "first loop " << i << '\n';
      auto& exprInfo = exprInfos[i];
      auto& copyInfo = exprInfo.copyInfo;
      auto* original = exprInfo.original;
      originalIndexes[original] = i;
      auto iter = seen.find(original);
      if (iter != seen.end()) {
std::cout << "  seen, append\n";
        // We have seen this before. Note it is a copy of the last of the
        // previous copies.
        auto& previous = iter->second;
        assert(!previous.empty());
        copyInfo.copyOf = previous;
        previous.push_back(original);
      } else {
std::cout << "  novel\n";
        // We've never seen this before. Add it.
        seen[original].push_back(original);
      }

      // Pop any children and see if we are part of a copy that
      // includes them.
      auto numChildren = ChildIterator(exprInfo.original).getNumChildren();
      copyInfo.fullSize = 1;
      for (Index child = 0; child < numChildren; child++) {
std::cout << "  child " << child << "\n";
        assert(!stack.empty());
        auto childInfo = stack.back();
        stack.pop_back();

        // For us to be a copy of something, we need to have found a shallow
        // copy of ourselves, and also for all of our children to appear in the
        // right positions as children of that previous appearance. Once
        // anything is not perfectly aligned, we have failed to find a copy.
        // We basically convert copyInfo.copyOf from a list of shallow copy
        // locations to a list of full copy locations, which is a subset of that
        // (and maybe empty). FIXME refactor
        SmallVector<Expression*, 1> filteredCopiesOf;
        for (auto copy : copyInfo.copyOf) {
std::cout << "    childCopy1, copy=" << originalIndexes[copy] << " , copyInfo.copyOf=" << copyInfo.copyOf.size() << " , copyInfo.fullSize=" << copyInfo.fullSize << "\n";
          // The child's location is our own plus a shift of the
          // size we've seen so far. That is, the first child is right before
          // us in the vector, and the one before it is at an additiona offset
          // of the size of the last child, and so forth.
          // Check if this child has a copy, and that copy is perfectly aligned
          // with the parent that we found ourselves to be a shallow copy of.
          for (auto childCopy : childInfo.copyOf) {
std::cout << "    childCopy2 " << originalIndexes[childCopy] << "  vs  " << (originalIndexes[copy] - copyInfo.fullSize) << "\n";
            if (originalIndexes[childCopy] == originalIndexes[copy] - copyInfo.fullSize) {
std::cout << "    childCopy3\n";
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

      if (!copyInfo.copyOf.empty() && isRelevant(original)) {
        foundRelevantCopy = true;
      }

      stack.push_back(copyInfo);
    }

    if (!foundRelevantCopy) {
      return;
    }
std::cout << "phase 2\n";

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

    auto* module = getModule();
    auto& passOptions = getPassOptions();
    Builder builder(*module);

    for (int i = int(exprInfos.size()) - 1; i >= 0; i--) {
      auto& currInfo = exprInfos[i]; // rename to info or currInfo?
      auto& copyInfo = currInfo.copyInfo;
      auto* curr = currInfo.original;

      if (copyInfo.copyOf.empty()) {
        continue;
      }

      if (!isRelevant(curr)) {
        // This has a copy, but it is not relevant to optimize. (We mus still
        // track such things as copies as their parents may be relevant.)
        continue;
      }

      // The index of the first child, or if there is no such child, the same
      // value as i. This is the beginning of the range of indexes that includes
      // the expression and all its children.
      auto firstChild = i - int(copyInfo.fullSize) + 1;
      assert(firstChild >= 0 && firstChild <= i);

      // When we optimize we replace all of this expression and its children
      // with a local.get. We can't do that if we've added a tee anywhere among
      // the children - we'd be removing the tee that is read later. That is, any
      // already-performed modification to this must be handled carefully, and
      // for now we just skip this case.
      //
      // We *can* easily handle the case of the entire expression, as opposed to
      // one of the children: we can simply move the tee to the copy before us,
      // which we handle later down. So we only exit here based on the children.
      //
      // TODO We can optimize this to an even earlier
      // copy with some extra care, or perhaps we can just do another
      // cycle.
      bool modified = false;
      for (int j = firstChild + 1; j <= i; j++) {
        if (exprInfos[j].addedTee) {
          modified = true;
          break;
        }
      }

      if (modified) {
        continue;
      }

      // Should we prefer the last perhaps?
      // Perhaps with the first we can assert on not needing to reuse a new
      // local added, as we'd always add the local at the very start of it all?
      auto* source = copyInfo.copyOf[0];

      // There is a copy. See if the first dominates the second, as if not then
      // we can just skip.
      if (!dominationChecker.dominates(source, curr)) {
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
      if (effects.hasSideEffects() ||
          Properties::isGenerative(curr, module->features)) {
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
      if (!dominationChecker.dominatesWithoutInterference(source,
                                                          curr,
                                                          effects,
                                                          ignoreEffectsOf,
                                                          *module,
                                                          passOptions)) {
        continue;
      }

      // Everything looks good! We can optimize here.
      auto& sourceInfo = exprInfos[originalIndexes[source]];
      if (!sourceInfo.addedTee) {
        // This is the first time we add a tee on the source in order to use its
        // value later (that is, we've not found another copy of |source|
        // earlier), so add a new local and add a tee.
        //
        // If |curr| already has a tee placed on it - which can happen if it is
        // the source for some appearance we've already optimized - then we've
        // already allocated a local, and can reuse that. Note that this is safe
        // to do as the previous copy is dominated by |curr|, and |curr| is
        // dominated by |source|, so |source| dominates the previous copy.
        Index tempLocal;
        if (currInfo.addedTee) {
          tempLocal = (*currInfo.currp)->cast<LocalSet>()->index;
        } else {
          tempLocal = builder.addVar(getFunction(), curr->type);
        }
        *sourceInfo.currp = builder.makeLocalTee(tempLocal, sourceInfo.original, curr->type);
        sourceInfo.addedTee = true;
      }
      *currInfo.currp = builder.makeLocalGet(
        (*sourceInfo.currp)->cast<LocalSet>()->index, curr->type);

      // Skip over all of our children before the next loop iteration, since we
      // have optimized the entire expression, including them, into a single
      // local.get.
      int childrenSize = copyInfo.fullSize - 1;
      // < and not <= becauase not only the children exist, but also the copy
      // before us that we optimize to.
      assert(childrenSize < i);
      i -= childrenSize;
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
