/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
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

/*
This is an optimized Go implemention of the Relooper algorithm originally
developed as part of Emscripten. This implementation includes optimizations
added since the original academic paper [1] was published about it. It was
derived from the C++ version that has been developed and improved in Binaryen.

[1] Alon Zakai. 2011. Emscripten: an LLVM-to-JavaScript compiler. In Proceedings
of the ACM international conference companion on Object oriented programming
systems languages and applications companion (SPLASH '11). ACM, New York, NY,
USA, 301-312. DOI=10.1145/2048147.2048224
http://doi.acm.org/10.1145/2048147.2048224
*/

package main

import "fmt"

// Represents code. This is opaque to the relooper, which doesn't need to know
// about the contents.
type Code interface {
}

type id int32

type RelooperBuilder interface {
  // Generate code to get the local variable used as the block label.
  makeGetLabel() *Code
  // Generate code to set the local variable used as the block label.
  makeSetLabel(*Code) *Code
  // Generate code to check if the local variable used as the block label has
  // a particular value.
  makeCheckLabel(id) *Code
  // Generate code to break to a specific block.
  makeBlockBreak(id) *Code
  // Generate code to continue to a specific loop.
  makeShapeContinue(id) *Code
  // Get the name of a block's target.
  getBlockBreakName(id) string
  // Get the name of a loop's target.
  getShapeContinueName(id) string
}

type FlowType int

const (
   // We will directly reach the right location through other
              // means, no need for continue or break
  Direct FlowType = iota
  Break
  Continue
)

// Info about a branching from one block to another.
type Branch struct {
  // If not NULL, this shape is the relevant one for purposes of getting to the
  // target block. We break or continue on it.
  Ancestor *Shape
  // If Ancestor is not NULL, this says whether to break or continue
  Type FlowType

  // A branch either has a condition expression if the block ends in ifs, or if
  // the block ends in a switch, then a list of indexes, which becomes the
  // indexes in the table of the switch. If not a switch, the condition can be
  // any expression (or nullptr for the branch taken when no other condition is
  // true) A condition must not have side effects, as the Relooper can reorder
  // or eliminate condition checking. This must not have side effects.
  Condition *Code
  // Switches are rare, so have just a pointer for their values. This contains
  // the values for which the branch will be taken, or for the default it is
  // simply not present.
  SwitchValues []int32

  // If provided, code that is run right before the branch is taken. This is
  // useful for phis.
  Code Code
}

type BlockSet map[*Block]bool
type BlockBranchMap map[*Block]*Branch

// Represents a basic block of code - some instructions that end with a
// control flow modifier (a branch, return or throw).
type Block struct {
  // Reference to the relooper containing this block.
  relooper *Relooper
  // Branches become processed after we finish the shape relevant to them. For
  // example, when we recreate a loop, branches to the loop start become
  // continues and are now processed. When we calculate what shape to generate
  // from a set of blocks, we ignore processed branches. Blocks own the Branch
  // objects they use, and destroy them when done.
  BranchesOut BlockBranchMap
  BranchesIn BlockSet
  ProcessedBranchesOut BlockBranchMap
  ProcessedBranchesIn BlockSet
  Parent *Shape
  // A unique identifier, defined when added to relooper
  Id int // TODO = -1
  // The code in this block. This can be arbitrary wasm code, including internal
  // control flow, it should just not branch to the outside
  Code *Code
  // If nullptr, then this block ends in ifs (or nothing). otherwise, this block
  // ends in a switch, done on this condition
  SwitchCondition *Code
  // If true, we are a multiple entry, so reaching us requires setting the label
  // variable
  IsCheckedMultipleEntry bool
}

// Represents a structured control flow shape, one of
//
//  Simple: No control flow at all, just instructions in a single
//          basic block.
//
//  Multiple: A shape with at least one entry. We may visit one of
//            the entries, or none, before continuing to the next
//            shape after this.
//
//  Loop: An infinite loop. We assume the property that a loop
//        will always visit one of its entries, and so for example
//        we cannot have a loop containing a multiple and nothing
//        else (since we might not visit any of the multiple's
//        blocks). Multiple entries are possible for the block,
//        however, which is necessary for irreducible control
//        flow, of course.
//

type ShapeType int

const (
  Simple ShapeType = iota
  Multiple
  Loop
)

type Shape struct {
  // A unique identifier. Used to identify loops, labels are Lx where x is the
  // Id. Defined when added to relooper
  Id int // TODO = -1
  // The shape that will appear in the code right after this one
  Next *Shape
  // The shape that control flow gets to naturally (if there is Next, then this
  // is Next)
  Natural *Shape

  Type ShapeType
}

type SimpleShape struct {
  Shape
  Inner *Block
}

type IdShapeMap [int]*Shape

type MultipleShape struct {
  Shape
  InnerMap IdShapeMap // entry block ID -> shape
}

type LoopShape struct {
  Shape
  Inner *Shape

  Entries BlockSet // we must visit at least one of these
}

// Implements the relooper algorithm for a function's blocks.
//
// Usage:
//  1. Instantiate this struct.
//  2. Create the blocks you have. Each should have its
//     branchings in specified (the branchings out will
//     be calculated by the relooper).
//  3. Call Render().
//
// Implementation details: The Relooper instance takes ownership of the blocks,
// branches and shapes when created using the `AddBlock` etc. methods, and frees
// them when done.
struct Relooper {
  wasm::Module* Module
  std::deque<std::unique_ptr<Block>> Blocks
  std::deque<std::unique_ptr<Branch>> Branches
  std::deque<std::unique_ptr<Shape>> Shapes
  Shape* Root
  bool MinSize
  int BlockIdCounter
  int ShapeIdCounter

  Relooper(wasm::Module* ModuleInit)

  // Creates a new block associated with (and cleaned up along) this relooper.
  Block* AddBlock(wasm::Expression* CodeInit,
                  wasm::Expression* SwitchConditionInit = nullptr)
  // Creates a new branch associated with (and cleaned up along) this relooper.
  Branch* AddBranch(wasm::Expression* ConditionInit,
                    wasm::Expression* CodeInit)
  // Creates a new branch associated with (and cleaned up along) this relooper.
  Branch* AddBranch(std::vector<wasm::Index>&& ValuesInit,
                    wasm::Expression* CodeInit = nullptr)
  // Creates a new simple shape associated with (and cleaned up along) this
  // relooper.
  SimpleShape* AddSimpleShape()
  // Creates a new multiple shape associated with (and cleaned up along) this
  // relooper.
  MultipleShape* AddMultipleShape()
  // Creates a new loop shape associated with (and cleaned up along) this
  // relooper.
  LoopShape* AddLoopShape()

  // Calculates the shapes
  void Calculate(Block* Entry)

  // Renders the result.
  wasm::Expression* Render(RelooperBuilder& Builder)

  // Sets us to try to minimize size
  void SetMinSize(bool MinSize_) { MinSize = MinSize_; }
}

typedef InsertOrderedMap<Block*, BlockSet> BlockBlockSetMap

#ifdef RELOOPER_DEBUG
struct Debugging {
  static void Dump(Block* Curr, const char* prefix = NULL)
  static void Dump(BlockSet& Blocks, const char* prefix = NULL)
  static void Dump(Shape* S, const char* prefix = NULL)
}
#endif

} // namespace CFG

// TODO: the .cpp file

func main() {
	fmt.Println("Hello, loops")
}




  // Add a branch: if the condition holds we branch (or if null, we branch if
  // all others failed) Note that there can be only one branch from A to B (if
  // you need multiple conditions for the branch, create a more interesting
  // expression in the Condition). If a Block has no outgoing branches, the
  // contents in Code must contain a terminating instruction, as the relooper
  // doesn't know whether you want control flow to stop with an `unreachable` or
  // a `return` or something else (if you forget to do this, control flow may
  // continue into the block that happens to be emitted right after it).
  // Internally, adding a branch only adds the outgoing branch. The matching
  // incoming branch on the target is added by the Relooper itself as it works.
  void AddBranchTo(Block* Target,
                   wasm::Expression* Condition,
                   wasm::Expression* Code = nullptr)

  // Add a switch branch: if the switch condition is one of these values, we
  // branch (or if the list is empty, we are the default) Note that there can be
  // only one branch from A to B (if you need multiple values for the branch,
  // that's what the array and default are for).
  void AddSwitchBranchTo(Block* Target,
                         std::vector<wasm::Index>&& Values,
                         wasm::Expression* Code = nullptr)

  // Emit code for the block, including its contents and branchings out
  wasm::Expression* Render(RelooperBuilder& Builder, bool InLoop)


  Shape(ShapeType TypeInit) : Type(TypeInit) {}
  virtual ~Shape() = default

  virtual wasm::Expression* Render(RelooperBuilder& Builder, bool InLoop) = 0

  static SimpleShape* IsSimple(Shape* It) {
    return It && It->Type == Simple ? (SimpleShape*)It : NULL
  }
  static MultipleShape* IsMultiple(Shape* It) {
    return It && It->Type == Multiple ? (MultipleShape*)It : NULL
  }
  static LoopShape* IsLoop(Shape* It) {
    return It && It->Type == Loop ? (LoopShape*)It : NULL
  }

