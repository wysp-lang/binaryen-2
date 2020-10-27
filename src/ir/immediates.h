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

#include <support/small_vector.h>
#include <wasm.h>
#include <wasm-traversal.h>

namespace wasm {

//
// Allows visiting the immediate fields of the expression. This is
// useful for comparisons and hashing.
//
// The passed-in visitor object must implement:
//  * visitScopeName - a Name that represents a block or loop scope
//  * visitNonScopeName - a non-scope name
//  * visitBool, etc. - a simple int type.
//  * visitLiteral - a Literal
//  * visitType - a Type
//  * visitSignature - a Signature
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
      visitor.visitBool(curr->isReturn);
    }
    void visitCallIndirect(CallIndirect* curr) {
      visitor.visitSignature(curr->sig);
      visitor.visitBool(curr->isReturn);
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
      visitor.visitUint8(curr->bytes);
      if (curr->type != Type::unreachable &&
          curr->bytes < curr->type.getByteSize()) {
        visitor.visitBool(curr->signed_);
      }
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
      visitor.visitBool(curr->isAtomic);
    }
    void visitStore(Store* curr) {
      visitor.visitUint8(curr->bytes);
      visitor.visitAddress(curr->offset);
      visitor.visitAddress(curr->align);
      visitor.visitBool(curr->isAtomic);
      visitor.visitType(curr->valueType);
    }
    void visitAtomicRMW(AtomicRMW* curr) {
      visitor.visitInt(curr->op);
      visitor.visitUint8(curr->bytes);
      visitor.visitAddress(curr->offset);
    }
    void visitAtomicCmpxchg(AtomicCmpxchg* curr) {
      visitor.visitUint8(curr->bytes);
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
    void visitI31Get(I31Get* curr) { visitor.visitBool(curr->signed_); }
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

// A simple visitor for immediates that accumulates them to type-specific
// vectors.
struct Immediates {
  SmallVector<Name, 1> scopeNames;
  SmallVector<Name, 1> nonScopeNames;
  SmallVector<bool, 3> bools;
  SmallVector<uint8_t, 3> uint8s;
  SmallVector<int32_t, 3> ints;
  SmallVector<uint64_t, 3> uint64s;
  SmallVector<Literal, 1> literals;
  SmallVector<Type, 1> types;
  SmallVector<Signature, 1> sigs;
  SmallVector<Index, 1> indexes;
  SmallVector<Address, 2> addresses;

  void visitScopeName(Name curr) { scopeNames.push_back(curr); }
  void visitNonScopeName(Name curr) { nonScopeNames.push_back(curr); }
  void visitBool(bool curr) { bools.push_back(curr); }
  void visitUint8(uint8_t curr) { uint8s.push_back(curr); }
  void visitInt(int32_t curr) { ints.push_back(curr); }
  void visitUint64(uint64_t curr) { uint64s.push_back(curr); }
  void visitLiteral(Literal curr) { literals.push_back(curr); }
  void visitType(Type curr) { types.push_back(curr); }
  void visitSignature(Signature curr) { sigs.push_back(curr); }
  void visitIndex(Index curr) { indexes.push_back(curr); }
  void visitAddress(Address curr) { addresses.push_back(curr); }
};

// A visitor that accumulates pointers to the immediates.
struct ImmediatePointers {
  SmallVector<Name*, 1> scopeNames;
  SmallVector<Name*, 1> nonScopeNames;
  SmallVector<bool*, 3> bools;
  SmallVector<uint8_t*, 3> uint8s;
  SmallVector<int32_t*, 3> ints;
  SmallVector<uint64_t*, 3> uint64s;
  SmallVector<Literal*, 1> literals;
  SmallVector<Type*, 1> types;
  SmallVector<Signature*, 1> sigs;
  SmallVector<Index*, 1> indexes;
  SmallVector<Address*, 2> addresses;

  void visitScopeName(Name& curr) { scopeNames.push_back(&curr); }
  void visitNonScopeName(Name& curr) { nonScopeNames.push_back(&curr); }
  void visitBool(bool& curr) { bools.push_back(&curr); }
  void visitUint8(uint8_t& curr) { uint8s.push_back(&curr); }
  void visitInt(int32_t& curr) { ints.push_back(&curr); }
  void visitUint64(uint64_t& curr) { uint64s.push_back(&curr); }
  void visitLiteral(Literal& curr) { literals.push_back(&curr); }
  void visitType(Type& curr) { types.push_back(&curr); }
  void visitSignature(Signature& curr) { sigs.push_back(&curr); }
  void visitIndex(Index& curr) { indexes.push_back(&curr); }
  void visitAddress(Address& curr) { addresses.push_back(&curr); }
};

} // namespace wasm

#endif // wasm_ir_immediates_h
