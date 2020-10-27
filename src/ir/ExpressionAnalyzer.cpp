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

#include "ir/immediates.h"
#include "ir/iteration.h"
#include "ir/load-utils.h"
#include "ir/utils.h"
#include "support/hash.h"
#include "support/small_vector.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

// Given a stack of expressions, checks if the topmost is used as a result.
// For example, if the parent is a block and the node is before the last
// position, it is not used.
bool ExpressionAnalyzer::isResultUsed(ExpressionStack& stack, Function* func) {
  for (int i = int(stack.size()) - 2; i >= 0; i--) {
    auto* curr = stack[i];
    auto* above = stack[i + 1];
    // only if and block can drop values (pre-drop expression was added) FIXME
    if (curr->is<Block>()) {
      auto* block = curr->cast<Block>();
      for (size_t j = 0; j < block->list.size() - 1; j++) {
        if (block->list[j] == above) {
          return false;
        }
      }
      assert(block->list.back() == above);
      // continue down
    } else if (curr->is<If>()) {
      auto* iff = curr->cast<If>();
      if (above == iff->condition) {
        return true;
      }
      if (!iff->ifFalse) {
        return false;
      }
      assert(above == iff->ifTrue || above == iff->ifFalse);
      // continue down
    } else {
      if (curr->is<Drop>()) {
        return false;
      }
      return true; // all other node types use the result
    }
  }
  // The value might be used, so it depends on if the function returns
  return func->sig.results != Type::none;
}

// Checks if a value is dropped.
bool ExpressionAnalyzer::isResultDropped(ExpressionStack& stack) {
  for (int i = int(stack.size()) - 2; i >= 0; i--) {
    auto* curr = stack[i];
    auto* above = stack[i + 1];
    if (curr->is<Block>()) {
      auto* block = curr->cast<Block>();
      for (size_t j = 0; j < block->list.size() - 1; j++) {
        if (block->list[j] == above) {
          return false;
        }
      }
      assert(block->list.back() == above);
      // continue down
    } else if (curr->is<If>()) {
      auto* iff = curr->cast<If>();
      if (above == iff->condition) {
        return false;
      }
      if (!iff->ifFalse) {
        return false;
      }
      assert(above == iff->ifTrue || above == iff->ifFalse);
      // continue down
    } else {
      if (curr->is<Drop>()) {
        return true; // dropped
      }
      return false; // all other node types use the result
    }
  }
  return false;
}

bool ExpressionAnalyzer::flexibleEqual(Expression* left,
                                       Expression* right,
                                       ExprComparer comparer) {
  struct Comparer {
    // for each name on the left, the corresponding name on the right
    std::map<Name, Name> rightNames;
    std::vector<Expression*> leftStack;
    std::vector<Expression*> rightStack;

    struct ComparableImmediates : public Immediates {
      Comparer& parent;

      ComparableImmediates(Comparer& parent) : parent(parent) {}

      // Comparison is by value, except for names, which must match.
      bool operator==(const ComparableImmediates& other) {
        if (scopeNames.size() != other.scopeNames.size()) {
          return false;
        }
        for (Index i = 0; i < scopeNames.size(); i++) {
          auto leftName = scopeNames[i];
          auto rightName = other.scopeNames[i];
          auto iter = parent.rightNames.find(leftName);
          // If it's not found, that means it was defined out of the expression
          // being compared, in which case we can just treat it literally - it
          // must be exactly identical.
          if (iter != parent.rightNames.end()) {
            leftName = iter->second;
          }
          if (leftName != rightName) {
            return false;
          }
        }
        if (nonScopeNames != other.nonScopeNames) {
          return false;
        }
        if (ints != other.ints) {
          return false;
        }
        if (literals != other.literals) {
          return false;
        }
        if (types != other.types) {
          return false;
        }
        if (indexes != other.indexes) {
          return false;
        }
        if (addresses != other.addresses) {
          return false;
        }
        return true;
      }

      bool operator!=(const ComparableImmediates& other) { return !(*this == other); }
    };

    bool noteNames(Name left, Name right) {
      if (left.is() != right.is()) {
        return false;
      }
      if (left.is()) {
        assert(rightNames.find(left) == rightNames.end());
        rightNames[left] = right;
      }
      return true;
    }

    bool compare(Expression* left, Expression* right, ExprComparer comparer) {
      // The empty name is the same on both sides.
      rightNames[Name()] = Name();

      leftStack.push_back(left);
      rightStack.push_back(right);

      while (leftStack.size() > 0 && rightStack.size() > 0) {
        left = leftStack.back();
        leftStack.pop_back();
        right = rightStack.back();
        rightStack.pop_back();
        if (!left != !right) {
          return false;
        }
        if (!left) {
          continue;
        }
        if (comparer(left, right)) {
          continue; // comparison hook, before all the rest
        }
        // continue with normal structural comparison
        if (left->_id != right->_id) {
          return false;
        }
        // Blocks and loops introduce scoping.
        if (auto* block = left->dynCast<Block>()) {
          if (!noteNames(block->name, right->cast<Block>()->name)) {
            return false;
          }
        } else if (auto* loop = left->dynCast<Loop>()) {
          if (!noteNames(loop->name, right->cast<Loop>()->name)) {
            return false;
          }
        } else {
          // For all other nodes, compare their immediate values
          ComparableImmediates leftImmediates(*this), rightImmediates(*this);
          visitImmediates(left, leftImmediates);
          visitImmediates(right, rightImmediates);
          if (leftImmediates != rightImmediates) {
            return false;
          }
        }
        // Add child nodes.
        Index counter = 0;
        for (auto* child : ChildIterator(left)) {
          leftStack.push_back(child);
          counter++;
        }
        for (auto* child : ChildIterator(right)) {
          rightStack.push_back(child);
          counter--;
        }
        // The number of child nodes must match (e.g. return has an optional
        // one).
        if (counter != 0) {
          return false;
        }
      }
      if (leftStack.size() > 0 || rightStack.size() > 0) {
        return false;
      }
      return true;
    }
  };

  return Comparer().compare(left, right, comparer);
}

// hash an expression, ignoring superficial details like specific internal names
size_t ExpressionAnalyzer::hash(Expression* curr) {
  struct Hasher {
    size_t digest = wasm::hash(0);

    Index internalCounter = 0;
    // for each internal name, its unique id
    std::map<Name, Index> internalNames;
    ExpressionStack stack;

    void noteScopeName(Name curr) {
      if (curr.is()) {
        internalNames[curr] = internalCounter++;
      }
    }

    Hasher(Expression* curr) {
      stack.push_back(curr);

      while (stack.size() > 0) {
        curr = stack.back();
        stack.pop_back();
        if (!curr) {
          continue;
        }
        rehash(digest, curr->_id);
        // we often don't need to hash the type, as it is tied to other values
        // we are hashing anyhow, but there are exceptions: for example, a
        // local.get's type is determined by the function, so if we are
        // hashing only expression fragments, then two from different
        // functions may turn out the same even if the type differs. Likewise,
        // if we hash between modules, then we need to take int account
        // call_imports type, etc. The simplest thing is just to hash the
        // type for all of them.
        rehash(digest, curr->type.getID());
        // Blocks and loops introduce scoping.
        if (auto* block = curr->dynCast<Block>()) {
          noteScopeName(block->name);
        } else if (auto* loop = curr->dynCast<Loop>()) {
          noteScopeName(loop->name);
        } else {
          // For all other nodes, compare their immediate values
          visitImmediates(curr, *this);
        }
        // Hash children
        Index counter = 0;
        for (auto* child : ChildIterator(curr)) {
          stack.push_back(child);
          counter++;
        }
        // Sometimes children are optional, e.g. return, so we must hash
        // their number as well.
        rehash(digest, counter);
      }
    }

    void visitScopeName(Name curr) {
      // Names are relative, we give the same hash for
      // (block $x (br $x))
      // (block $y (br $y))
      static_assert(sizeof(Index) == sizeof(int32_t),
                    "wasm64 will need changes here");
      assert(internalNames.find(curr) != internalNames.end());
      rehash(digest, internalNames[curr]);
    }
    void visitNonScopeName(Name curr) { rehash(digest, uint64_t(curr.str)); }
    void visitBool(bool curr) { rehash(digest, curr); }
    void visitUint8(uint8_t curr) { rehash(digest, curr); }
    void visitInt(int32_t curr) { rehash(digest, curr); }
    void visitUint64(uint64_t curr) { rehash(digest, curr); }
    void visitLiteral(Literal curr) { rehash(digest, curr); }
    void visitType(Type curr) { rehash(digest, curr.getID()); }
    void visitSignature(Signature curr) {
      rehash(digest, curr.params.getID());
      rehash(digest, curr.results.getID());
    }
    void visitIndex(Index curr) {
      static_assert(sizeof(Index) == sizeof(uint32_t),
                    "wasm64 will need changes here");
      rehash(digest, curr);
    }
    void visitAddress(Address curr) { rehash(digest, curr.addr); }
  };

  return Hasher(curr).digest;
}

} // namespace wasm
