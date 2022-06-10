/*
 * Copyright 2018 WebAssembly Community Group participants
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
// Operations on Stack IR.
//

#include "ir/iteration.h"
#include "ir/local-graph.h"
#include "pass.h"
#include "wasm-stack.h"
#include "wasm.h"

namespace wasm {

// Generate Stack IR from Binaryen IR

struct GenerateStackIR : public WalkerPass<PostWalker<GenerateStackIR>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new GenerateStackIR; }

  bool modifiesBinaryenIR() override { return false; }

  void doWalkFunction(Function* func) {
    StackIRGenerator stackIRGen(*getModule(), func);
    stackIRGen.write();
    func->stackIR = make_unique<StackIR>();
    func->stackIR->swap(stackIRGen.getStackIR());
  }
};

Pass* createGenerateStackIRPass() { return new GenerateStackIR(); }

// Optimize

class StackIROptimizer {
  Function* func;
  PassOptions& passOptions;
  StackIR& insts;
  FeatureSet features;

public:
  StackIROptimizer(Function* func,
                   PassOptions& passOptions,
                   FeatureSet features)
    : func(func), passOptions(passOptions), insts(*func->stackIR.get()),
      features(features) {
    assert(func->stackIR);
  }

  void run() {
    dce();
    // FIXME: local2Stack is currently rather slow (due to localGraph),
    //        so for now run it only when really optimizing
    if (passOptions.optimizeLevel >= 3 || passOptions.shrinkLevel >= 1) {
      local2Stack();
    }
    // Removing unneeded blocks is dangerous with GC, as if we do this:
    //
    //   (call
    //     (rtt)
    //     (block
    //       (nop)
    //       (i32)
    //     )
    //   )
    // === remove inner block ==>
    //   (call
    //     (rtt)
    //     (nop)
    //     (i32)
    //   )
    //
    // Then we end up with a nop that forces us to emit this during load:
    //
    //   (call
    //     (block
    //       (local.set
    //         (rtt)
    //       )
    //       (nop)
    //       (local.get)
    //     )
    //     (i32)
    //   )
    //
    // However, that is not valid as an rtt cannot be set to a local.
    if (!features.hasGC()) {
      removeUnneededBlocks();
    }
    dce();
    countNNLocalAnnotations();
  }

private:
  // Passes.

  // Remove unreachable code.
  void dce() {
    bool inUnreachableCode = false;
    for (Index i = 0; i < insts.size(); i++) {
      auto* inst = insts[i];
      if (!inst) {
        continue;
      }
      if (inUnreachableCode) {
        // Does the unreachable code end here?
        if (isControlFlowBarrier(inst)) {
          inUnreachableCode = false;
        } else {
          // We can remove this.
          removeAt(i);
        }
      } else if (inst->type == Type::unreachable) {
        inUnreachableCode = true;
      }
    }
  }

  // If ordered properly, we can avoid a local.set/local.get pair,
  // and use the value directly from the stack, for example
  //    [..produce a value on the stack..]
  //    local.set $x
  //    [..much code..]
  //    local.get $x
  //    call $foo ;; use the value, foo(value)
  // As long as the code in between does not modify $x, and has
  // no control flow branching out, we can remove both the set
  // and the get.
  void local2Stack() {
    // We use the localGraph to tell us if a get-set pair is indeed
    // a set that is read by that get, and only that get. Note that we run
    // this on the Binaryen IR, so we are assuming that no previous opt
    // has changed the interaction of local operations.
    // TODO: we can do this a lot faster, as we just care about linear
    //       control flow.
    LocalGraph localGraph(func);
    localGraph.computeSetInfluences();
    // We maintain a stack of relevant values. This contains:
    //  * a null for each actual value that the value stack would have
    //  * an index of each LocalSet that *could* be on the value
    //    stack at that location.
    const Index null = -1;
    std::vector<Index> values;
    // We also maintain a stack of values vectors for control flow,
    // saving the stack as we enter and restoring it when we exit.
    std::vector<std::vector<Index>> savedValues;
#ifdef STACK_OPT_DEBUG
    std::cout << "func: " << func->name << '\n' << insts << '\n';
#endif
    for (Index i = 0; i < insts.size(); i++) {
      auto* inst = insts[i];
      if (!inst) {
        continue;
      }
      // First, consume values from the stack as required.
      auto consumed = getNumConsumedValues(inst);
#ifdef STACK_OPT_DEBUG
      std::cout << "  " << i << " : " << *inst << ", " << values.size()
                << " on stack, will consume " << consumed << "\n    ";
      for (auto s : values)
        std::cout << s << ' ';
      std::cout << '\n';
#endif
      // TODO: currently we run dce before this, but if we didn't, we'd need
      //       to handle unreachable code here - it's ok to pop multiple values
      //       there even if the stack is at size 0.
      while (consumed > 0) {
        assert(values.size() > 0);
        // Whenever we hit a possible stack value, kill it - it would
        // be consumed here, so we can never optimize to it.
        while (values.back() != null) {
          values.pop_back();
          assert(values.size() > 0);
        }
        // Finally, consume the actual value that is consumed here.
        values.pop_back();
        consumed--;
      }
      // After consuming, we can see what to do with this. First, handle
      // control flow.
      if (isControlFlowBegin(inst)) {
        // Save the stack for when we end this control flow.
        savedValues.push_back(values); // TODO: optimize copies
        values.clear();
      } else if (isControlFlowEnd(inst)) {
        assert(!savedValues.empty());
        values = savedValues.back();
        savedValues.pop_back();
      } else if (isControlFlow(inst)) {
        // Otherwise, in the middle of control flow, just clear it
        values.clear();
      }
      // This is something we should handle, look into it.
      if (inst->type.isConcrete()) {
        bool optimized = false;
        if (auto* get = inst->origin->dynCast<LocalGet>()) {
          // This is a potential optimization opportunity! See if we
          // can reach the set.
          if (values.size() > 0) {
            Index j = values.size() - 1;
            while (1) {
              // If there's an actual value in the way, we've failed.
              auto index = values[j];
              if (index == null) {
                break;
              }
              auto* set = insts[index]->origin->cast<LocalSet>();
              if (set->index == get->index) {
                // This might be a proper set-get pair, where the set is
                // used by this get and nothing else, check that.
                auto& sets = localGraph.getSetses[get];
                if (sets.size() == 1 && *sets.begin() == set) {
                  auto& setInfluences = localGraph.setInfluences[set];
                  if (setInfluences.size() == 1) {
                    assert(*setInfluences.begin() == get);
                    // Do it! The set and the get can go away, the proper
                    // value is on the stack.
#ifdef STACK_OPT_DEBUG
                    std::cout << "  stackify the get\n";
#endif
                    insts[index] = nullptr;
                    insts[i] = nullptr;
                    // Continuing on from here, replace this on the stack
                    // with a null, representing a regular value. We
                    // keep possible values above us active - they may
                    // be optimized later, as they would be pushed after
                    // us, and used before us, so there is no conflict.
                    values[j] = null;
                    optimized = true;
                    break;
                  }
                }
              }
              // We failed here. Can we look some more?
              if (j == 0) {
                break;
              }
              j--;
            }
          }
        }
        if (!optimized) {
          // This is an actual regular value on the value stack.
          values.push_back(null);
        }
      } else if (inst->origin->is<LocalSet>() && inst->type == Type::none) {
        // This set is potentially optimizable later, add to stack.
        values.push_back(i);
      }
    }
  }

  // There may be unnecessary blocks we can remove: blocks
  // without branches to them are always ok to remove.
  // TODO: a branch to a block in an if body can become
  //       a branch to that if body
  void removeUnneededBlocks() {
    for (auto*& inst : insts) {
      if (!inst) {
        continue;
      }
      if (auto* block = inst->origin->dynCast<Block>()) {
        if (!BranchUtils::BranchSeeker::has(block, block->name)) {
          // TODO optimize, maybe run remove-unused-names
          inst = nullptr;
        }
      }
    }
  }

  // Utilities.

  // A control flow "barrier" - a point where stack machine
  // unreachability ends.
  bool isControlFlowBarrier(StackInst* inst) {
    switch (inst->op) {
      case StackInst::BlockEnd:
      case StackInst::IfElse:
      case StackInst::IfEnd:
      case StackInst::LoopEnd:
      case StackInst::Catch:
      case StackInst::CatchAll:
      case StackInst::Delegate:
      case StackInst::TryEnd: {
        return true;
      }
      default: { return false; }
    }
  }

  // A control flow beginning.
  bool isControlFlowBegin(StackInst* inst) {
    switch (inst->op) {
      case StackInst::BlockBegin:
      case StackInst::IfBegin:
      case StackInst::LoopBegin:
      case StackInst::TryBegin: {
        return true;
      }
      default: { return false; }
    }
  }

  // A control flow "middle" (a separator inside a control flow structure).
  bool isControlFlowMiddle(StackInst* inst) {
    switch (inst->op) {
      case StackInst::IfElse:
      case StackInst::Catch:
      case StackInst::CatchAll:
      case StackInst::Delegate: {
        return true;
      }
      default: { return false; }
    }
  }

  // A control flow ending.
  bool isControlFlowEnd(StackInst* inst) {
    switch (inst->op) {
      case StackInst::BlockEnd:
      case StackInst::IfEnd:
      case StackInst::LoopEnd:
      case StackInst::TryEnd:
      case StackInst::Delegate: {
        return true;
      }
      default: { return false; }
    }
  }

  bool isControlFlow(StackInst* inst) { return inst->op != StackInst::Basic; }

  // Remove the instruction at index i. If the instruction
  // is control flow, and so has been expanded to multiple
  // instructions, remove them as well.
  void removeAt(Index i) {
    auto* inst = insts[i];
    insts[i] = nullptr;
    if (inst->op == StackInst::Basic) {
      return; // that was it
    }
    auto* origin = inst->origin;
    while (1) {
      i++;
      assert(i < insts.size());
      inst = insts[i];
      insts[i] = nullptr;
      if (inst && inst->origin == origin && isControlFlowEnd(inst)) {
        return; // that's it, we removed it all
      }
    }
  }

  Index getNumConsumedValues(StackInst* inst) {
    if (isControlFlow(inst)) {
      // If consumes 1; that's it.
      if (inst->op == StackInst::IfBegin) {
        return 1;
      }
      return 0;
    }
    // Otherwise, for basic instructions, just count the expression children.
    return ChildIterator(inst->origin).children.size();
  }

  void countNNLocalAnnotations() {
    std::cout << func->name << "..\n";
    // Estimate the cost of non-nullable local encoding of written locals
    // annotations. A block/loop/if/try must be annotated with a local if that
    // local is defined in them, it is used after them, and it was not already
    // defined before them. For example:
    //
    //  (block $A
    //    (block $B
    //      (block $C
    //        ..
    //        local.set $x
    //        (block $D
    //          ..
    //          local.set $x
    //        )
    //        local.get $x
    //        ..
    //      )
    //    )
    //    local.get $x
    //    ..
    //  )
    //
    // * $A does not need to annotate $x since it is not used afterwards.
    // * $B and C do need to annotate.
    // * $D does not need to annotate $x since it was already defined before $D.
    //
    // To compute this, first we find the most pessimistic representation, where
    // any local.set of a non-nullable local leads to annotation on the control
    // flow structure it is in, and annotations are propagated outwards. We will
    // later prune that.

    using Locals = std::unordered_set<Index>;

    // Maps indexes in |insts| to the annotations present there. This is only
    // relevant for control flow structures, so it is almost always very sparse
    // (and maybe should be a map and not a vector?). The annotation is on the
    // index of the end of the control flow structure.
    std::vector<Locals> annotationsMap(insts.size());

    struct StructureInfo {
      Expression* origin;
      // The current "part" of the structure. For a block this is always 0, for
      // an if it is 0 in the first arm and 1 in the second, etc.
      Index currPart;
      // For each part, the non-nullable locals set there.
      std::vector<Locals> partLocals;

      StructureInfo(Expression* origin) : origin(origin) {}

      void startNewPart() {
        partLocals.emplace_back();
        currPart = partLocals.size() - 1;
      }

      void addPart(Locals locals) {
        partLocals.emplace_back(locals);
      }
    };

    // A stack of info on all structures at the current point in time.
    std::vector<StructureInfo> structureInfoStack;

    // The current part of the control flow structures on the stack. For a block
    // this is always 0, for an if it will be 0 in the first arm and 1 in the
    // second, etc.
    std::vector<Index> structureParts;

    size_t totalAnnotations = 0;

    for (Index i = 0; i < insts.size(); i++) {
//std::cout << "waka " << i << '\n';
      auto* inst = insts[i];
      if (!inst) {
        continue;
      }
      auto* origin = inst->origin;
      if (auto* set = origin->dynCast<LocalSet>()) {
//std::cout << "  waka a\n";
        auto index = set->index;
        if (func->getLocalType(index).isNonNullable()) {
//std::cout << "  waka b\n";
          if (!structureInfoStack.empty()) {
//std::cout << "  waka c\n";
            auto& info = structureInfoStack.back();
            assert(info.currPart < info.partLocals.size());
            info.partLocals[info.currPart].insert(index);
          }
        }
        continue;
      }
      if (Properties::isBranch(origin)) {
        // The current sets are sent to the target.
        assert(!structureInfoStack.empty());
        assert(!structureInfoStack.back().partLocals.empty());
        auto& currLocals = structureInfoStack.back().partLocals.back();
        for (auto target : BranchUtils::getUniqueTargets(origin)) {
          for (auto& targetInfo : structureInfoStack) {
            // We don't need to send stuff to loops, as they go to the top of
            // the loop, and not out.
            if (auto* targetBlock = targetInfo.origin->dynCast<Block>()) {
              if (targetBlock->name == target) {
                targetInfo.addPart(currLocals);
                break;
              }
            }
          }
        }
      }
      if (isControlFlowBegin(inst)) {
//std::cout << "  waka begin\n";
        structureInfoStack.emplace_back(origin);
        structureInfoStack.back().startNewPart();
        continue;
      }
      if (isControlFlowMiddle(inst)) {
//std::cout << "  waka middle\n";
        assert(!structureInfoStack.empty());
        structureInfoStack.back().startNewPart();
        continue;
      }
      if (isControlFlowEnd(inst)) {
//std::cout << "  waka end\n";
        assert(!structureInfoStack.empty());
        auto& info = structureInfoStack.back();
        if (auto* iff = origin->dynCast<If>()) {
          if (!iff->ifFalse) {
            // An if without an else has an implicit empty arm.
            info.startNewPart();
          }
        }
        auto& annotations = annotationsMap[i];
        assert(annotations.empty());
        assert(!info.partLocals.empty());
        if (info.partLocals.size() == 1) {
          annotations = info.partLocals[0];
        } else {
          // This has multiple parts. The final set of locals is their
          // intersection.
          //
          // Compute that with a map of index to how many parts it appears in.
          std::vector<Index> counts(func->getNumLocals());
          auto numParts = info.partLocals.size();
          for (auto& part : info.partLocals) {
            for (auto index : part) {
              assert(index < counts.size());
              counts[index]++;
              assert(counts[index] <= numParts);
              if (counts[index] == numParts) {
                annotations.insert(index);
              }
            }
          }
        }
        totalAnnotations += annotations.size();
        structureInfoStack.pop_back();
        // Propagate our annotations to the outside.
        if (!structureInfoStack.empty()) {
          auto& info = structureInfoStack.back();
          assert(info.currPart < info.partLocals.size());
          auto& partLocals = info.partLocals[info.currPart];
          for (auto index : annotations) {
            partLocals.insert(index);
          }
        }
        continue;
      }
    }
    assert(structureInfoStack.empty());
    std::cout << func->name << " : " << totalAnnotations << '\n';
  }
};

struct OptimizeStackIR : public WalkerPass<PostWalker<OptimizeStackIR>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new OptimizeStackIR; }

  bool modifiesBinaryenIR() override { return false; }

  void doWalkFunction(Function* func) {
    if (!func->stackIR) {
      return;
    }
    StackIROptimizer(func, getPassOptions(), getModule()->features).run();
  }
};

Pass* createOptimizeStackIRPass() { return new OptimizeStackIR(); }

} // namespace wasm
