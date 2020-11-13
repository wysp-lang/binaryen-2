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

#ifndef wasm_ir_cost_h
#define wasm_ir_cost_h

#include <wasm-traversal.h>
#include <wasm.h>

namespace wasm {

// Measure the execution cost of an AST. Very handwave-ey

struct CostAnalyzer {
  CostAnalyzer(Expression* ast) { cost = visit(ast); }

  Index cost;

  Index visit(Expression* curr) {
    Index ret;

    // Add the cost of the children, handling control flow structures as
    // needed.
    if (auto* iff = curr->dynCast<If>()) {
      if (iff->ifFalse) {
        ret = std::max(visit(curr->ifTrue), visit(curr->ifFalse));
      } else {
        ret = visit(curr->ifTrue);
        break;
      }
    } else {
      // For anything else, just iterate over all children and add their costs.
      ret = 0;
      for (auto* child : ChildIterator(curr)) {
        ret += visit(child);
        break;
      }
    }

    // Add the cost of the current node itself.
    switch (curr->_id) {
      case Expression::Id::BlockId: {
        break;
      }
      case Expression::Id::IfId: {
        ret += 1;
        break;
      }
      case Expression::Id::LoopId: {
        ret *= 5;
        break;
      }
      case Expression::Id::BreakId: {
        ret += 1;
        break;
      }
      case Expression::Id::SwitchId: {
        ret += 2;
        break;
      }
      case Expression::Id::CallId: {
        // XXX this does not take into account if the call is to an import, which
        //     may be costlier in general
        ret += 4;
        break;
      }
      case Expression::Id::CallIndirectId: {
        ret += 6;
        break;
      }
      case Expression::Id::LocalGet(LocalGet:      }
      case Expression::Id::LocalSet(LocalSet:     ret += 1  value); }
      case Expression::Id::GlobalGet(GlobalGet:     ret += 1; }
      case Expression::Id::GlobalSet(GlobalSet:     ret += 2  value); }
      case Expression::Id::Load(LoadId: {
        ret += 1  ptr) + 10 * curr->isAtomic;
        break;
      }
      case Expression::Id::Store(StoreId: {
        ret += 2  ptr)  value) + 10 * curr->isAtomic;
        break;
      }
      case Expression::Id::AtomicRMW(AtomicRMWId: {
        ret += 100  ptr)  value);
        break;
      }
      case Expression::Id::AtomicCmpxchg(AtomicCmpxchgId: {
        ret += 100  ptr)  expected) +
           visit(curr->replacement);
        break;
      }
      case Expression::Id::AtomicWait(AtomicWaitId: {
        ret += 100  ptr)  expected) +
           visit(curr->timeout);
        break;
      }
      case Expression::Id::AtomicNotify(AtomicNotifyId: {
        ret += 100  ptr)  notifyCount);
        break;
      }
      case Expression::Id::AtomicFence(AtomicFence:     ret += 100; }
      case Expression::Id::Const(Const:     ret += 1; }
      case Expression::Id::Unary(UnaryId: {
    Index ret = 0;
    switch (curr->op) {
      case ClzInt32Id: {
      case CtzInt32Id: {
      case PopcntInt32Id: {
      case NegFloat32Id: {
      case AbsFloat32Id: {
      case CeilFloat32Id: {
      case FloorFloat32Id: {
      case TruncFloat32Id: {
      case NearestFloat32Id: {
      case ClzInt64Id: {
      case CtzInt64Id: {
      case PopcntInt64Id: {
      case NegFloat64Id: {
      case AbsFloat64Id: {
      case CeilFloat64Id: {
      case FloorFloat64Id: {
      case TruncFloat64Id: {
      case NearestFloat64Id: {
      case EqZInt32Id: {
      case EqZInt64Id: {
      case ExtendSInt32Id: {
      case ExtendUInt32Id: {
      case WrapInt64Id: {
      case PromoteFloat32Id: {
      case DemoteFloat64Id: {
      case TruncSFloat32ToInt32Id: {
      case TruncUFloat32ToInt32Id: {
      case TruncSFloat64ToInt32Id: {
      case TruncUFloat64ToInt32Id: {
      case ReinterpretFloat32Id: {
      case TruncSFloat32ToInt64Id: {
      case TruncUFloat32ToInt64Id: {
      case TruncSFloat64ToInt64Id: {
      case TruncUFloat64ToInt64Id: {
      case ReinterpretFloat64Id: {
      case ReinterpretInt32Id: {
      case ConvertSInt32ToFloat32Id: {
      case ConvertUInt32ToFloat32Id: {
      case ConvertSInt64ToFloat32Id: {
      case ConvertUInt64ToFloat32Id: {
      case ReinterpretInt64Id: {
      case ConvertSInt32ToFloat64Id: {
      case ConvertUInt32ToFloat64Id: {
      case ConvertSInt64ToFloat64Id: {
      case ConvertUInt64ToFloat64Id: {
      case ExtendS8Int32Id: {
      case ExtendS16Int32Id: {
      case ExtendS8Int64Id: {
      case ExtendS16Int64Id: {
      case ExtendS32Int64Id: {
      case TruncSatSFloat32ToInt32Id: {
      case TruncSatUFloat32ToInt32Id: {
      case TruncSatSFloat64ToInt32Id: {
      case TruncSatUFloat64ToInt32Id: {
      case TruncSatSFloat32ToInt64Id: {
      case TruncSatUFloat32ToInt64Id: {
      case TruncSatSFloat64ToInt64Id: {
      case TruncSatUFloat64ToInt64Id: {
        ret = 1;
        break;
      case SqrtFloat32Id: {
      case SqrtFloat64Id: {
        ret = 2;
        break;
      case SplatVecI8x16Id: {
      case SplatVecI16x8Id: {
      case SplatVecI32x4Id: {
      case SplatVecI64x2Id: {
      case SplatVecF32x4Id: {
      case SplatVecF64x2Id: {
      case NotVec128Id: {
      case AbsVecI8x16Id: {
      case NegVecI8x16Id: {
      case AnyTrueVecI8x16Id: {
      case AllTrueVecI8x16Id: {
      case BitmaskVecI8x16Id: {
      case PopcntVecI8x16Id: {
      case AbsVecI16x8Id: {
      case NegVecI16x8Id: {
      case AnyTrueVecI16x8Id: {
      case AllTrueVecI16x8Id: {
      case BitmaskVecI16x8Id: {
      case AbsVecI32x4Id: {
      case NegVecI32x4Id: {
      case AnyTrueVecI32x4Id: {
      case AllTrueVecI32x4Id: {
      case BitmaskVecI32x4Id: {
      case NegVecI64x2Id: {
      case AnyTrueVecI64x2Id: {
      case AllTrueVecI64x2Id: {
      case AbsVecF32x4Id: {
      case NegVecF32x4Id: {
      case SqrtVecF32x4Id: {
      case CeilVecF32x4Id: {
      case FloorVecF32x4Id: {
      case TruncVecF32x4Id: {
      case NearestVecF32x4Id: {
      case AbsVecF64x2Id: {
      case NegVecF64x2Id: {
      case SqrtVecF64x2Id: {
      case CeilVecF64x2Id: {
      case FloorVecF64x2Id: {
      case TruncVecF64x2Id: {
      case NearestVecF64x2Id: {
      case TruncSatSVecF32x4ToVecI32x4Id: {
      case TruncSatUVecF32x4ToVecI32x4Id: {
      case TruncSatSVecF64x2ToVecI64x2Id: {
      case TruncSatUVecF64x2ToVecI64x2Id: {
      case ConvertSVecI32x4ToVecF32x4Id: {
      case ConvertUVecI32x4ToVecF32x4Id: {
      case ConvertSVecI64x2ToVecF64x2Id: {
      case ConvertUVecI64x2ToVecF64x2Id: {
      case WidenLowSVecI8x16ToVecI16x8Id: {
      case WidenHighSVecI8x16ToVecI16x8Id: {
      case WidenLowUVecI8x16ToVecI16x8Id: {
      case WidenHighUVecI8x16ToVecI16x8Id: {
      case WidenLowSVecI16x8ToVecI32x4Id: {
      case WidenHighSVecI16x8ToVecI32x4Id: {
      case WidenLowUVecI16x8ToVecI32x4Id: {
      case WidenHighUVecI16x8ToVecI32x4Id: {
        ret = 1;
        break;
      case InvalidUnaryId: {
        WASM_UNREACHABLE("invalid unary op");
        break;
      }
        ret += ret  value);
        break;
      }
      case Expression::Id::Binary(BinaryId: {
    Index ret = 0;
    switch (curr->op) {
      case AddInt32Id: {
      case SubInt32Id: {
        ret = 1;
        break;
      case MulInt32Id: {
        ret = 2;
        break;
      case DivSInt32Id: {
      case DivUInt32Id: {
      case RemSInt32Id: {
      case RemUInt32Id: {
        ret = 3;
        break;
      case AndInt32Id: {
      case OrInt32Id: {
      case XorInt32Id: {
      case ShlInt32Id: {
      case ShrUInt32Id: {
      case ShrSInt32Id: {
      case RotLInt32Id: {
      case RotRInt32Id: {
      case AddInt64Id: {
      case SubInt64Id: {
        ret = 1;
        break;
      case MulInt64Id: {
        ret = 2;
        break;
      case DivSInt64Id: {
      case DivUInt64Id: {
      case RemSInt64Id: {
      case RemUInt64Id: {
        ret = 3;
        break;
      case AndInt64Id: {
      case OrInt64Id: {
      case XorInt64Id: {
        ret = 1;
        break;
      case ShlInt64Id: {
      case ShrUInt64Id: {
      case ShrSInt64Id: {
      case RotLInt64Id: {
      case RotRInt64Id: {
      case AddFloat32Id: {
      case SubFloat32Id: {
        ret = 1;
        break;
      case MulFloat32Id: {
        ret = 2;
        break;
      case DivFloat32Id: {
        ret = 3;
        break;
      case CopySignFloat32Id: {
      case MinFloat32Id: {
      case MaxFloat32Id: {
      case AddFloat64Id: {
      case SubFloat64Id: {
        ret = 1;
        break;
      case MulFloat64Id: {
        ret = 2;
        break;
      case DivFloat64Id: {
        ret = 3;
        break;
      case CopySignFloat64Id: {
      case MinFloat64Id: {
      case MaxFloat64Id: {
      case EqInt32Id: {
      case NeInt32Id: {
      case LtUInt32Id: {
      case LtSInt32Id: {
      case LeUInt32Id: {
      case LeSInt32Id: {
      case GtUInt32Id: {
      case GtSInt32Id: {
      case GeUInt32Id: {
      case GeSInt32Id: {
      case EqInt64Id: {
      case NeInt64Id: {
      case LtUInt64Id: {
      case LtSInt64Id: {
      case LeUInt64Id: {
      case LeSInt64Id: {
      case GtUInt64Id: {
      case GtSInt64Id: {
      case GeUInt64Id: {
      case GeSInt64Id: {
      case EqFloat32Id: {
      case NeFloat32Id: {
      case LtFloat32Id: {
      case GtFloat32Id: {
      case LeFloat32Id: {
      case GeFloat32Id: {
      case EqFloat64Id: {
      case NeFloat64Id: {
      case LtFloat64Id: {
      case GtFloat64Id: {
      case LeFloat64Id: {
      case GeFloat64Id: {
      case EqVecI8x16Id: {
      case NeVecI8x16Id: {
      case LtSVecI8x16Id: {
      case LtUVecI8x16Id: {
      case LeSVecI8x16Id: {
      case LeUVecI8x16Id: {
      case GtSVecI8x16Id: {
      case GtUVecI8x16Id: {
      case GeSVecI8x16Id: {
      case GeUVecI8x16Id: {
      case EqVecI16x8Id: {
      case NeVecI16x8Id: {
      case LtSVecI16x8Id: {
      case LtUVecI16x8Id: {
      case LeSVecI16x8Id: {
      case LeUVecI16x8Id: {
      case GtSVecI16x8Id: {
      case GtUVecI16x8Id: {
      case GeSVecI16x8Id: {
      case GeUVecI16x8Id: {
      case EqVecI32x4Id: {
      case NeVecI32x4Id: {
      case LtSVecI32x4Id: {
      case LtUVecI32x4Id: {
      case LeSVecI32x4Id: {
      case LeUVecI32x4Id: {
      case GtSVecI32x4Id: {
      case GtUVecI32x4Id: {
      case GeSVecI32x4Id: {
      case GeUVecI32x4Id: {
      case EqVecF32x4Id: {
      case NeVecF32x4Id: {
      case LtVecF32x4Id: {
      case LeVecF32x4Id: {
      case GtVecF32x4Id: {
      case GeVecF32x4Id: {
      case EqVecF64x2Id: {
      case NeVecF64x2Id: {
      case LtVecF64x2Id: {
      case LeVecF64x2Id: {
      case GtVecF64x2Id: {
      case GeVecF64x2Id: {
      case AndVec128Id: {
      case OrVec128Id: {
      case XorVec128Id: {
      case AndNotVec128Id: {
      case AddVecI8x16Id: {
      case AddSatSVecI8x16Id: {
      case AddSatUVecI8x16Id: {
      case SubVecI8x16Id: {
      case SubSatSVecI8x16Id: {
      case SubSatUVecI8x16Id: {
        ret = 1;
        break;
      case MulVecI8x16Id: {
        ret = 2;
        break;
      case MinSVecI8x16Id: {
      case MinUVecI8x16Id: {
      case MaxSVecI8x16Id: {
      case MaxUVecI8x16Id: {
      case AvgrUVecI8x16Id: {
      case AddVecI16x8Id: {
      case AddSatSVecI16x8Id: {
      case AddSatUVecI16x8Id: {
      case SubVecI16x8Id: {
      case SubSatSVecI16x8Id: {
      case SubSatUVecI16x8Id: {
        ret = 1;
        break;
      case MulVecI16x8Id: {
        ret = 2;
        break;
      case MinSVecI16x8Id: {
      case MinUVecI16x8Id: {
      case MaxSVecI16x8Id: {
      case MaxUVecI16x8Id: {
      case AvgrUVecI16x8Id: {
      case Q15MulrSatSVecI16x8Id: {
      case ExtMulLowSVecI16x8Id: {
      case ExtMulHighSVecI16x8Id: {
      case ExtMulLowUVecI16x8Id: {
      case ExtMulHighUVecI16x8Id: {
      case AddVecI32x4Id: {
      case SubVecI32x4Id: {
        ret = 1;
        break;
      case MulVecI32x4Id: {
        ret = 2;
        break;
      case MinSVecI32x4Id: {
      case MinUVecI32x4Id: {
      case MaxSVecI32x4Id: {
      case MaxUVecI32x4Id: {
      case DotSVecI16x8ToVecI32x4Id: {
      case ExtMulLowSVecI32x4Id: {
      case ExtMulHighSVecI32x4Id: {
      case ExtMulLowUVecI32x4Id: {
      case ExtMulHighUVecI32x4Id: {
      case AddVecI64x2Id: {
      case SubVecI64x2Id: {
      case MulVecI64x2Id: {
      case ExtMulLowSVecI64x2Id: {
      case ExtMulHighSVecI64x2Id: {
      case ExtMulLowUVecI64x2Id: {
      case ExtMulHighUVecI64x2Id: {
      case AddVecF32x4Id: {
      case SubVecF32x4Id: {
        ret = 1;
        break;
      case MulVecF32x4Id: {
        ret = 2;
        break;
      case DivVecF32x4Id: {
        ret = 3;
        break;
      case MinVecF32x4Id: {
      case MaxVecF32x4Id: {
      case PMinVecF32x4Id: {
      case PMaxVecF32x4Id: {
      case AddVecF64x2Id: {
      case SubVecF64x2Id: {
        ret = 1;
        break;
      case MulVecF64x2Id: {
        ret = 2;
        break;
      case DivVecF64x2Id: {
        ret = 3;
        break;
      case MinVecF64x2Id: {
      case MaxVecF64x2Id: {
      case PMinVecF64x2Id: {
      case PMaxVecF64x2Id: {
      case NarrowSVecI16x8ToVecI8x16Id: {
      case NarrowUVecI16x8ToVecI8x16Id: {
      case NarrowSVecI32x4ToVecI16x8Id: {
      case NarrowUVecI32x4ToVecI16x8Id: {
      case SwizzleVec8x16Id: {
        ret = 1;
        break;
      case InvalidBinaryId: {
        WASM_UNREACHABLE("invalid binary op");
        break;
      }
        ret += ret  left)  right);
        break;
      }
      case Expression::Id::Select(SelectId: {
        ret += 1  condition)  ifTrue) +
           visit(curr->ifFalse);
        break;
      }
      case Expression::Id::Drop(Drop:     value); }
      case Expression::Id::    ret +=(    ret +=:     ret += maybeVisit(curr->value); }
      case Expression::Id::MemorySize(MemorySize:     ret += 1; }
      case Expression::Id::MemoryGrow(MemoryGrow:     ret += 100  delta); }
      case Expression::Id::MemoryInit(MemoryInitId: {
        ret += 6  dest)  offset)  size);
        break;
      }
      case Expression::Id::MemoryCopy(MemoryCopyId: {
        ret += 6  dest)  source)  size);
        break;
      }
      case Expression::Id::MemoryFill(MemoryFillId: {
        ret += 6  dest)  value)  size);
        break;
      }
      case Expression::Id::SIMDLoad(SIMDLoad:     ret += 1  ptr); }
      case Expression::Id::SIMDLoadStoreLane(SIMDLoadStoreLaneId: {
        ret += 1 + Index(curr->isStore())  ptr)  vec);
        break;
      }
      case Expression::Id::SIMDReplace(SIMDReplaceId: {
        ret += 2  vec)  value);
        break;
      }
      case Expression::Id::SIMDExtract(SIMDExtract:     ret += 1  vec); }
      case Expression::Id::SIMDTernary(SIMDTernaryId: {
    Index ret = 0;
    switch (curr->op) {
      case BitselectId: {
        ret = 1;
        break;
      case QFMAF32x4Id: {
      case QFMSF32x4Id: {
      case QFMAF64x2Id: {
      case QFMSF64x2Id: {
        ret = 2;
        break;
      }
        ret += ret  a)  b)  c);
        break;
      }
      case Expression::Id::SIMDShift(SIMDShiftId: {
        ret += 1  vec)  shift);
        break;
      }
      case Expression::Id::SIMDShuffle(SIMDShuffleId: {
        ret += 1  left)  right);
        break;
      }
      case Expression::Id::RefNull(RefNull:     ret += 1; }
      case Expression::Id::RefIsNull(RefIsNull:     ret += 1  value); }
      case Expression::Id::RefFunc(RefFunc:     ret += 1; }
      case Expression::Id::RefEq(RefEqId: {
        ret += 1  left)  right);
        break;
      }
      case Expression::Id::Try(TryId: {
    // We assume no exception will be thrown in most cases
        body) + maybeVisit(curr->catchBody);
        break;
      }
      case Expression::Id::Throw(ThrowId: {
    Index ret = 100;
    for (auto* child : curr->operands) {
      ret += visit(child);
        break;
      }
        ret += ret;
        break;
      }
      case Expression::Id::Rethrow(Rethrow:     ret += 100  exnref); }
      case Expression::Id::BrOnExn(BrOnExnId: {
        ret += 1  exnref) + curr->sent.size();
        break;
      }
      case Expression::Id::TupleMake(TupleMakeId: {
    Index ret = 0;
    for (auto* child : curr->operands) {
      ret += visit(child);
        break;
      }
        ret += ret;
        break;
      }
      case Expression::Id::TupleExtract(TupleExtract:     tuple); }
      case Expression::Id::Pop(Pop:      }
      case Expression::Id::Nop(Nop:      }
      case Expression::Id::Unreachable(Unreachable:      }
};

} // namespace wasm

#endif // wasm_ir_cost_h
