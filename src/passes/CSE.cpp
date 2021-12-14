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
//       We really need something like this, at least to match gets with tees
//       and through copies etc. - each local.get must be tracked to its single
//       source, and comparable. That is, when we see local.get $x then we
//       can get a link from it to the value of the single set/tee that is its
//       source, if there is one, and that is equal to it - a copy. And also
//       look through a tee, through fallthroughs, etc.
//       see cse-testcase.wat
//       we dont need LocalGraph here. It is ok to say that a local.get $x
//       compares equal to the last local.get $x, *and also* equal to the last
//       local.set/tee $x - since we will check for effects in the middle later
//       if necessary. the pass already does that for local.gets when it
//       compares them structurally. we can just extend that to local.set, by
//       making it and its value compare equal to local.gets. Say, making the
//       shallow compared value of local.set's value equal to a local.get of
//       that same local. Ignore tee, as that has side effects - we still need
//       untee - but otherwise, turn local.set values into local.gets for
//       purposes of comparison.
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
#include "ir/literal-utils.h"
#include "ir/local-graph.h"
#include "ir/numbering.h"
#include "ir/properties.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace std { // TODO: move to hash.h?

// Hashing vectors can be useful
template<typename T> struct hash<vector<T>> {
  size_t operator()(const vector<T>& v) const {
    auto digest = wasm::hash(v.size());
    for (auto& x : v) {
      wasm::rehash(digest, x);
    }
    return digest;
  }
};

} // namespace std

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
                                              HSEComparer>; // TODO needed?

// Performs a GVN analysis of a function, computing a value number for each
// expression and filling in data structures for use while optimizing with that
// info.
struct GVNAnalysis {
  // Maps a value number to the list of expressions that have that value number.
  std::vector<std::vector<Expression*>> exprsForValue;

  struct ExprInfo {
    Expression* expr;
    Expression** currp;
    Index number;
  };

  // Info for each expression in the function, in post-order ordering.
  std::vector<ExprInfo> exprInfos;

  std::unordered_map<Expression*, Index> exprNumbers;

  Module& wasm;

  GVNAnalysis(Function* func, Module& wasm) : wasm(wasm) {
    struct Computer
      : public PostWalker<Computer, UnifiedExpressionVisitor<Computer>> {
      GVNAnalysis& parent;

      Function* func;

      LocalGraph localGraph;

      std::vector<Index> numberStack;

      ValueNumbering numbering;

      // A vector of numbers of children. Optimized for the common case of 2 or
      // fewer children.
      using ChildNumbers = std::vector<Index>; // SmallVector<2>?

      // Maps vectors of child numbers to a number.
      using NumberVecMap = std::unordered_map<ChildNumbers, Index>;

      // Maps a shallow expression to NumberVecMap. Together, this gives a data
      // structure that lets us go from a shallow expression + the value numbers
      // of its children to associate a number with that combination.
      using ShallowExprNumberVecMap =
        std::unordered_map<HashedShallowExpression,
                           NumberVecMap,
                           HSEHasher,
                           HSEComparer>;

      ShallowExprNumberVecMap numbersMap;

      std::vector<Index> paramNumbers;

      Computer(GVNAnalysis& parent, Function* func)
        : parent(parent), func(func), localGraph(func) {
        for (Index i = 0; i < func->getNumParams(); i++) {
          paramNumbers.push_back(getNewNumber());
        }
      }

      void visitExpression(Expression* curr) {
        // Get the children's value numbers off the stack.
        auto numChildren = ChildIterator(curr).getNumChildren();
        ChildNumbers childNumbers;
        childNumbers.resize(numChildren);
        for (Index i = 0; i < numChildren; i++) {
          childNumbers[i] = numberStack.back();
          numberStack.pop_back();
        }

        // Compute the number of this expression.
        Index number;
        if (!curr->type.isConcrete()) {
          number = getNewNumber();
        }
        if (Properties::isShallowlyGenerative(curr, parent.wasm.features) ||
            Properties::isCall(curr)) {
          number = getNewNumber();
        } else if (auto* get = curr->dynCast<LocalGet>()) {
          number = getLocalGetNumber(get);
        } else if (auto* set = curr->dynCast<LocalSet>()) {
          // We've handled non-concrete types before, leaving only tee to handle
          // here.
          assert(set->isTee());
          assert(childNumbers.size() == 1);
          // A tee's value is simply that of its child.
          number = childNumbers[0];
        } else {
          // For anything else, compute a value number from the children's
          // numbers plus the shallow contents of the expression.
          NumberVecMap& numbersMapEntry = numbersMap[curr];
          Index& savedNumber = numbersMapEntry[childNumbers];
          if (savedNumber == 0) {
            // This is the first appearance.
            savedNumber = getNewNumber();
          }
          number = savedNumber;
        }

        // Now that we have computed the number of this expression, add it to
        // the data structures.
        numberStack.push_back(number);

        if (parent.exprsForValue.size() <= number) {
          parent.exprsForValue.resize(number + 1);
        }
        parent.exprsForValue[number].push_back(curr);

        parent.exprInfos.push_back(ExprInfo{curr, getCurrentPointer(), number});

        parent.exprNumbers[curr] = number; // TODO: needed beyond local.set?
      }

      Index getNewNumber() {
        auto number = numbering.getUniqueValue();
        assert(number > 0);
        return number;
      }

      Index getLocalGetNumber(LocalGet* get) {
        Index number;
        auto& sets = localGraph.getSetses[get];
        if (sets.size() != 1) {
          // TODO: we could do more here for merges
          number = getNewNumber();
        } else {
          auto* set = *sets.begin();
          if (!set) {
            // This is a param or a null initializer value.
            if (func->isParam(set->index)) {
              number = paramNumbers[set->index];
            } else {
              number = numbering.getValue(
                LiteralUtils::makeZero(func->getLocalType(set->index), parent.wasm));
            }
          } else {
            // We must have seen the value already in our traversal.
            auto* value = set->value;
            assert(parent.exprNumbers.count(value));
            number = parent.exprNumbers[value];
          }
        }
        parent.exprNumbers[get] = number;
        return number;
      }
    } computer(*this, func);
  }
};

struct GVNPass : public WalkerPass<GVNPass> {
  bool isFunctionParallel() override { return true; }

  // FIXME DWARF updating does not handle local changes yet.
  bool invalidatesDWARF() override { return true; }

  Pass* create() override { return new GVNPass(); }

  void doWalkFunction(Function* func) {
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

} // anonymous namespace

Pass* createCSEPass() { return new GVNPass(); }

} // namespace wasm

#pragma GCC diagnostic pop


/*

#if 0
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
    Properties::isShallowlyGenerative(original, module->features)) {
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
        if (previous.size() >= 10) previous.pop_back(); // keep it reasonable. past some limit, just replace the last (most recent) item
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
            // TODO: this loop will need to change for local.get opts, as the
            // get is smaller in general than the thing it is a copy of.
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
#endif



*/

