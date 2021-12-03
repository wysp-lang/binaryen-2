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
// TODO: use value numbering to get GVN
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

  struct ExprInfo {
    // The original expression at this location. It may be replaced later as we
    // optimize.
    Expression* original;

    // The pointer to the expression, for use if we need to replace the expr.
    Expression** currp;

    // The complete size of the expression, that is, including nested children.
    Index fullSize = -1;

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
    struct StackInfo {
      // If not -1 then this is the index of a copy of the expression that
      // appears before.
      // TODO: This could be a set of copies, which would let us optimize a few
      //       more cases. Right now we just store the last copy seen here.
      Index copyOf;

      // The complete size of the expression, that is, including nested children.
      Index fullSize;
    };
    std::vector<StackInfo> stack;

    // Track the shallow expressions that we've seen so far.
    HashedShallowExprs seen;

    // The indexes at which expressions reside. TODO this could be part of
    // the hashed expression perhaps, avoiding an extra map. That is, we could
    // store indexes while we hash.
    std::unordered_map<Expression*, Index> originalIndexes;

    for (Index i = 0; i < exprInfos.size(); i++) {
      auto* original = exprInfo.original;
      originalIndexes[original] = i;
      Index copyOf = -1;
      auto iter = seen.find(original);
      if (iter != seen.end()) {
        // We have seen this before. Note it is a copy of the last of the
        // previous copies.
        auto& previous = iter->second;
        assert(!previous.empty());
        copyOf = originalIndexes[previous.back()];
        previous.push_back(original);
      } else {
        // We've never seen this before. Add it.
        seen[original].push_back(original);
      }

      auto numChildren = ChildIterator(exprInfo.original).getNumChildren();
      if (numChildren == 0) {
        // This is of size one.
        stack.push_back(StackInfo(copyOf, 1));
      }
    }





    // Fix up any nondefaultable locals that we've added.
    TypeUpdating::handleNonDefaultableLocals(func, *getModule());
  }
};

Pass* createCSEPass() { return new CSE(); }

} // namespace wasm
