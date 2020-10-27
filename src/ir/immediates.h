/*
 * Copyright 2020 WebAssembly Community Group participants
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

#ifndef wasm_ir_immediates_h
#define wasm_ir_immediates_h

namespace wasm {

//
// Allows visiting the immediate fields of the expression. This is
// useful for comparisons and hashing.
//
// The passed-in visitor object must implement:
//  * visitScopeName - a Name that represents a block or loop scope
//  * visitNonScopeName - a non-scope name
//  * visitInt - anything that has a short enumeration, including
//               opcodes, # of bytes in a load, bools, etc. - must be
//               guaranteed to fit in an int32 or less.
//  * visitLiteral - a Literal
//  * visitType - a Type
//  * visitIndex - an Index
//  * visitAddress - an Address
//

template<typename T> void visitImmediates(Expression* curr, T& visitor) {
  struct ImmediateVisitor : public OverriddenVisitor<ImmediateVisitor> {
    T& visitor;

    ImmediateVisitor(Expression* curr, T& visitor) : visitor(visitor) {
      this->visit(curr);
    }

    void visitBlock(Block* curr) { visitor.visitScopeName(curr->name); }
    void visitIf(If* curr) {}
    void visitLoop(Loop* curr) { visitor.visitScopeName(curr->name); }
    void visitBreak(Break* curr) { visitor.visitScopeName(curr->name); }
    void visitSwitch(Switch* curr) {
      for (auto target : curr->targets) {
        visitor.visitScopeName(target);
      }
      visitor.visitScopeName(curr->default_);
    }
    void visitCall(Call* curr) {
      visitor.visitNonScopeName(curr->target);
      visitor.visitInt(curr->isReturn);
    }
    void visitCallIndirect(CallIndirect* curr) {
      visitor.visitInt(curr->sig.params.getID());
      visitor.visitInt(curr->sig.results.getID());
      visitor.visitInt(curr->isReturn);
    }
    void visitLocalGet(LocalGet* curr) { visitor.visitIndex(curr->index); }
    void visitLocalSet(LocalSet* curr) { visitor.visitIndex(curr->index); }
    void visitGlobalGet(GlobalGet* curr) {
      visitor.visitNonScopeName(curr->name);
    }
    void visitGlobalSet(GlobalSet* curr) {
      visitor.visitNonScopeName(curr->name);
    }
    void visitLoad(Load* curr) {
      visitor.visitInt(curr->bytes);
      if (curr->type != Type::unreachable &&
          curr->bytes < curr->type.getByteSize()) {
        visitor.visitInt(curr->signed_);
      }
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
      visitor.visitInt(curr->isAtomic);
    }
    void visitStore(Store* curr) {
      visitor.visitInt(curr->bytes);
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
      visitor.visitInt(curr->isAtomic);
      visitor.visitInt(curr->valueType.getID());
    }
    void visitAtomicRMW(AtomicRMW* curr) {
      visitor.visitInt(curr->op);
      visitor.visitInt(curr->bytes);
      visitor.visitAddress(curr->offset);
    }
    void visitAtomicCmpxchg(AtomicCmpxchg* curr) {
      visitor.visitInt(curr->bytes);
      visitor.visitAddress(curr->offset);
    }
    void visitAtomicWait(AtomicWait* curr) {
      visitor.visitAddress(curr->offset);
      visitor.visitType(curr->expectedType);
    }
    void visitAtomicNotify(AtomicNotify* curr) {
      visitor.visitAddress(curr->offset);
    }
    void visitAtomicFence(AtomicFence* curr) { visitor.visitInt(curr->order); }
    void visitSIMDExtract(SIMDExtract* curr) {
      visitor.visitInt(curr->op);
      visitor.visitInt(curr->index);
    }
    void visitSIMDReplace(SIMDReplace* curr) {
      visitor.visitInt(curr->op);
      visitor.visitInt(curr->index);
    }
    void visitSIMDShuffle(SIMDShuffle* curr) {
      for (auto x : curr->mask) {
        visitor.visitInt(x);
      }
    }
    void visitSIMDTernary(SIMDTernary* curr) { visitor.visitInt(curr->op); }
    void visitSIMDShift(SIMDShift* curr) { visitor.visitInt(curr->op); }
    void visitSIMDLoad(SIMDLoad* curr) {
      visitor.visitInt(curr->op);
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
    }
    void visitSIMDLoadStoreLane(SIMDLoadStoreLane* curr) {
      visitor.visitInt(curr->op);
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
      visitor.visitInt(curr->index);
    }
    void visitMemoryInit(MemoryInit* curr) {
      visitor.visitIndex(curr->segment);
    }
    void visitDataDrop(DataDrop* curr) { visitor.visitIndex(curr->segment); }
    void visitMemoryCopy(MemoryCopy* curr) {}
    void visitMemoryFill(MemoryFill* curr) {}
    void visitConst(Const* curr) { visitor.visitLiteral(curr->value); }
    void visitUnary(Unary* curr) { visitor.visitInt(curr->op); }
    void visitBinary(Binary* curr) { visitor.visitInt(curr->op); }
    void visitSelect(Select* curr) {}
    void visitDrop(Drop* curr) {}
    void visitReturn(Return* curr) {}
    void visitMemorySize(MemorySize* curr) {}
    void visitMemoryGrow(MemoryGrow* curr) {}
    void visitRefNull(RefNull* curr) { visitor.visitType(curr->type); }
    void visitRefIsNull(RefIsNull* curr) {}
    void visitRefFunc(RefFunc* curr) { visitor.visitNonScopeName(curr->func); }
    void visitRefEq(RefEq* curr) {}
    void visitTry(Try* curr) {}
    void visitThrow(Throw* curr) { visitor.visitNonScopeName(curr->event); }
    void visitRethrow(Rethrow* curr) {}
    void visitBrOnExn(BrOnExn* curr) {
      visitor.visitScopeName(curr->name);
      visitor.visitNonScopeName(curr->event);
    }
    void visitNop(Nop* curr) {}
    void visitUnreachable(Unreachable* curr) {}
    void visitPop(Pop* curr) {}
    void visitTupleMake(TupleMake* curr) {}
    void visitTupleExtract(TupleExtract* curr) {
      visitor.visitIndex(curr->index);
    }
    void visitI31New(I31New* curr) {}
    void visitI31Get(I31Get* curr) { visitor.visitInt(curr->signed_); }
    void visitRefTest(RefTest* curr) {
      WASM_UNREACHABLE("TODO (gc): ref.test");
    }
    void visitRefCast(RefCast* curr) {
      WASM_UNREACHABLE("TODO (gc): ref.cast");
    }
    void visitBrOnCast(BrOnCast* curr) {
      WASM_UNREACHABLE("TODO (gc): br_on_cast");
    }
    void visitRttCanon(RttCanon* curr) {
      WASM_UNREACHABLE("TODO (gc): rtt.canon");
    }
    void visitRttSub(RttSub* curr) { WASM_UNREACHABLE("TODO (gc): rtt.sub"); }
    void visitStructNew(StructNew* curr) {
      WASM_UNREACHABLE("TODO (gc): struct.new");
    }
    void visitStructGet(StructGet* curr) {
      WASM_UNREACHABLE("TODO (gc): struct.get");
    }
    void visitStructSet(StructSet* curr) {
      WASM_UNREACHABLE("TODO (gc): struct.set");
    }
    void visitArrayNew(ArrayNew* curr) {
      WASM_UNREACHABLE("TODO (gc): array.new");
    }
    void visitArrayGet(ArrayGet* curr) {
      WASM_UNREACHABLE("TODO (gc): array.get");
    }
    void visitArraySet(ArraySet* curr) {
      WASM_UNREACHABLE("TODO (gc): array.set");
    }
    void visitArrayLen(ArrayLen* curr) {
      WASM_UNREACHABLE("TODO (gc): array.len");
    }
  } singleton(curr, visitor);
}

} // namespace wasm

#endif // wasm_ir_immediates_h
