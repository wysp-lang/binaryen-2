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

#ifndef wasm_ir_linear_execution_h
#define wasm_ir_linear_execution_h

#include <ir/properties.h>
#include <wasm-traversal.h>
#include <wasm.h>

namespace wasm {

//
// Traversal in the order of execution, with hooks for controlling behavior at
// join and merge points. This uses wasm's structured control flow efficiently
// to do certain cross-basic-block operations, but is not still a single
// postorder pass and so it is not a replacement for basic blocks in general.
//
// All merge and split opertations are performed on a templated data type. For
// example, given an if, control flow will split into the arms, then merge back
// after them. And if the data type is, say, a set of interesting locals,
// splitting might copy the set to the arms, and merging might intersect them,
// etc.
//
// Subclass should implement the two hooks, noteSplit and noteMerge. The former
// provides a pointer to data and a vector of targets we are splitting to, while
// the latter provides the vector of sources and a single target. Split is
// called right when we split from the current execution location, and merge
// right when we reach the code where a merge occurs (that is, when we reach a
// Break we just note its values; only when we reach the Break target do we
// perform a merge.
//
// This does not handle loops. XXX
//
template<typename SubType, typename DataType, typename VisitorType = Visitor<SubType>>
struct StructuredExecutionWalker : public PostWalker<SubType, VisitorType> {
  StructuredExecutionWalker() = default;

  using std::unique_ptr<DataType> = DataPtr;

  void noteSplit(const DataPtr& source,
                 const std::vector<DataPtr>& targets) { abort(); }
  void noteMerge(const std::vector<DataPtr>& sources,
                 const DataPtr& target) { abort(); }

  // Create the data when we enter a new basic block. Subclasses can optionally
  // override this.
  void makeCurrData() {
    currData = std::make_unique<DataType>();
  }

  // The current data as we walk.
  DataPtr currData;

  std::unordered_map<Name, 

  static void scan(SubType* self, Expression** currp) {
    Expression* curr = *currp;

    switch (curr->_id) {
      case Expression::Id::InvalidId:
        WASM_UNREACHABLE("bad id");
      case Expression::Id::BlockId: {
        self->pushTask(SubType::doVisitBlock, currp);
        if (curr->cast<Block>()->name.is()) {
          self->pushTask(SubType::doNoteNonLinear, currp);
        }
        auto& list = curr->cast<Block>()->list;
        for (int i = int(list.size()) - 1; i >= 0; i--) {
          self->pushTask(SubType::scan, &list[i]);
        }
        break;
      }
      case Expression::Id::IfId: {
        self->pushTask(SubType::doVisitIf, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->maybePushTask(SubType::scan, &curr->cast<If>()->ifFalse);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->pushTask(SubType::scan, &curr->cast<If>()->ifTrue);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->pushTask(SubType::scan, &curr->cast<If>()->condition);
        break;
      }
      case Expression::Id::LoopId: {
        self->pushTask(SubType::doVisitLoop, currp);
        self->pushTask(SubType::scan, &curr->cast<Loop>()->body);
        self->pushTask(SubType::doNoteNonLinear, currp);
        break;
      }
      case Expression::Id::BreakId: {
        self->pushTask(SubType::doVisitBreak, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->maybePushTask(SubType::scan, &curr->cast<Break>()->condition);
        self->maybePushTask(SubType::scan, &curr->cast<Break>()->value);
        break;
      }
      case Expression::Id::SwitchId: {
        self->pushTask(SubType::doVisitSwitch, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->pushTask(SubType::scan, &curr->cast<Switch>()->condition);
        self->maybePushTask(SubType::scan, &curr->cast<Switch>()->value);
        break;
      }
      case Expression::Id::ReturnId: {
        self->pushTask(SubType::doVisitReturn, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->maybePushTask(SubType::scan, &curr->cast<Return>()->value);
        break;
      }
      case Expression::Id::TryId: {
        self->pushTask(SubType::doVisitTry, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        auto& list = curr->cast<Try>()->catchBodies;
        for (int i = int(list.size()) - 1; i >= 0; i--) {
          self->pushTask(SubType::scan, &list[i]);
          self->pushTask(SubType::doNoteNonLinear, currp);
        }
        self->pushTask(SubType::scan, &curr->cast<Try>()->body);
        break;
      }
      case Expression::Id::ThrowId: {
        self->pushTask(SubType::doVisitThrow, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        auto& list = curr->cast<Throw>()->operands;
        for (int i = int(list.size()) - 1; i >= 0; i--) {
          self->pushTask(SubType::scan, &list[i]);
        }
        break;
      }
      case Expression::Id::RethrowId: {
        self->pushTask(SubType::doVisitRethrow, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        break;
      }
      case Expression::Id::UnreachableId: {
        self->pushTask(SubType::doVisitUnreachable, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        break;
      }
      case Expression::Id::BrOnId: {
        self->pushTask(SubType::doVisitBrOn, currp);
        self->pushTask(SubType::doNoteNonLinear, currp);
        self->pushTask(SubType::scan, &curr->cast<BrOn>()->ref);
        break;
      }
      default: {
        // All relevant things should have been handled.
        assert(!Properties::isControlFlowStructure(curr));
        assert(!Properties::isBranch(curr));
        // other node types do not have control flow, use regular post-order
        PostWalker<SubType, VisitorType>::scan(self, currp);
      }
    }
  }
};

} // namespace wasm

#endif // wasm_ir_linear_execution_h
