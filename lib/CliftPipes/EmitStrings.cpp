//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <limits>
#include <optional>
#include <string>

#include "llvm/ADT/SmallVector.h"

#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "revng/Clift/Clift.h"
#include "revng/Clift/CliftOpHelpers.h"
#include "revng/Clift/CliftTypes.h"
#include "revng/Clift/LocationAddresses.h"
#include "revng/CliftPipes/EmitStrings.h"
#include "revng/Model/Binary.h"
#include "revng/SegmentReferences/StringConstants.h"
#include "revng/Support/Debug.h"

using namespace clift;

static Logger Log("emit-strings");

namespace {

/// Returns the meta-address of a global object unambiguously referred to by the
/// specified expression. The expression must be composed by a use operation
/// followed by any number of direct member access operations. Any other kind of
/// subexpression is considered ambiguous and returns an invalid meta-address.
static MetaAddress getGlobalObjectMetaAddress(mlir::Value Value) {
  uint64_t Offset = 0;

  while (true) {
    mlir::Operation *Op = Value.getDefiningOp();
    if (Op == nullptr)
      return MetaAddress::invalid();

    if (auto E = mlir::dyn_cast<UseOp>(Op)) {
      auto Global = mlir::cast<GlobalVariableOp>(E.getUsedGlobal());

      auto Location = pipeline::locationFromString(revng::ranks::Segment,
                                                   Global.getHandle());
      if (not Location)
        return MetaAddress::invalid();

      return std::get<0>(Location->at(revng::ranks::Segment)) + Offset;
    }

    if (auto E = mlir::dyn_cast<DirectAccessOp>(Op)) {
      Value = E.getValue();
      Offset += E.getFieldAttr().getOffset();
      continue;
    }

    return MetaAddress::invalid();
  }
}

static std::optional<int64_t> checkedAdd(int64_t LHS, int64_t RHS) {
  int64_t Result;
  if (__builtin_add_overflow(LHS, RHS, &Result))
    return std::nullopt;
  return Result;
}

static std::optional<int64_t> checkedSub(int64_t LHS, int64_t RHS) {
  int64_t Result;
  if (__builtin_sub_overflow(LHS, RHS, &Result))
    return std::nullopt;
  return Result;
}

static std::optional<int64_t> checkedMul(int64_t LHS, int64_t RHS) {
  int64_t Result;
  if (__builtin_mul_overflow(LHS, RHS, &Result))
    return std::nullopt;
  return Result;
}

static std::optional<int64_t> getSignedOffset(mlir::Value V) {
  if (auto Imm = V.getDefiningOp<ImmediateOp>()) {
    auto IntType = unwrapped_dyn_cast<IntegerType>(Imm.getType());
    if (not IntType)
      return std::nullopt;

    const llvm::APInt &Value = Imm.getValue();
    if (IntType.getKind() == IntegerKind::Signed) {
      if (Value.getSignificantBits() > 64)
        return std::nullopt;
      return Value.getSExtValue();
    }

    if (Value.getActiveBits() > 63)
      return std::nullopt;
    return static_cast<int64_t>(Value.getZExtValue());
  }

  if (auto Neg = V.getDefiningOp<NegOp>()) {
    auto Inner = getSignedOffset(Neg.getValue());
    if (not Inner or *Inner == std::numeric_limits<int64_t>::min())
      return std::nullopt;
    return -*Inner;
  }

  return std::nullopt;
}

/// Returns true only when every use of \p Access stays within the \p Size bytes
/// of the string object and does not rely on its address relative to the
/// enclosing segment. Replacing the segment field access with a standalone
/// string literal detaches it from the segment, so out-of-bounds reads,
/// dynamic pointer arithmetic, and segment-relative address arithmetic must
/// keep the original segment access.
static bool staysWithinString(DirectAccessOp Access, uint64_t Size) {
  constexpr uint64_t MaxInt64 = std::numeric_limits<int64_t>::max();
  auto IsInBounds = [Size](int64_t Offset, uint64_t AccessSize) -> bool {
    if (Offset < 0)
      return false;
    uint64_t UnsignedOffset = static_cast<uint64_t>(Offset);
    if (UnsignedOffset > Size)
      return false;
    return AccessSize <= Size - UnsignedOffset;
  };

  struct UseState {
    mlir::Value Value;
    int64_t Offset;
    bool IsLvalue;
  };

  llvm::SmallVector<UseState> Worklist;
  Worklist.push_back({ Access.getResult(), 0, true });

  while (not Worklist.empty()) {
    UseState State = Worklist.pop_back_val();

    for (mlir::OpOperand &Use : State.Value.getUses()) {
      mlir::Operation *Op = Use.getOwner();

      if (State.IsLvalue) {
        if (mlir::isa<AddressofOp, DecayOp>(Op)) {
          Worklist.push_back({ Op->getResult(0), State.Offset, false });
          continue;
        }

        if (auto FieldAccess = mlir::dyn_cast<DirectAccessOp>(Op)) {
          uint64_t FieldOffset = FieldAccess.getFieldAttr().getOffset();
          if (FieldOffset > MaxInt64)
            return false;
          auto NewOffset = checkedAdd(State.Offset,
                                      static_cast<int64_t>(FieldOffset));
          if (not NewOffset)
            return false;
          Worklist.push_back({ FieldAccess.getResult(), *NewOffset, true });
          continue;
        }

        if (auto Comma = mlir::dyn_cast<CommaOp>(Op)) {
          if (Use.getOperandNumber() == 1) {
            Worklist.push_back({ Comma.getResult(), State.Offset, true });
            continue;
          }
          uint64_t ObjSize = getObjectSizeOrZero(State.Value.getType());
          if (ObjSize == 0 or not IsInBounds(State.Offset, ObjSize))
            return false;
          continue;
        }

        if (auto Assign = mlir::dyn_cast<AssignOp>(Op)) {
          if (Use.getOperandNumber() == 0)
            return false;
        } else if (mlir::isa<IncrementOp,
                             DecrementOp,
                             PostIncrementOp,
                             PostDecrementOp>(Op)) {
          return false;
        }

        // Any other use performs an lvalue-to-rvalue read (or yields the
        // object) of its full type size at the current offset.
        uint64_t ObjSize = getObjectSizeOrZero(State.Value.getType());
        if (ObjSize == 0 or not IsInBounds(State.Offset, ObjSize))
          return false;
        continue;
      }

      if (mlir::isa<BitCastOp, ImplicitCastOp, PtrResizeOp>(Op)) {
        Worklist.push_back({ Op->getResult(0), State.Offset, false });
        continue;
      }

      if (auto PtrAdd = mlir::dyn_cast<PtrAddOp>(Op)) {
        if (PtrAdd.getPointer() != State.Value)
          return false;
        auto PtrType = unwrapped_dyn_cast<PointerType>(State.Value.getType());
        uint64_t ElemSize = PtrType ?
                              getObjectSizeOrZero(PtrType.getPointeeType()) :
                              0;
        if (ElemSize == 0 or ElemSize > MaxInt64)
          return false;
        auto Step = getSignedOffset(PtrAdd.getOffset());
        if (not Step)
          return false;
        auto Delta = checkedMul(*Step, static_cast<int64_t>(ElemSize));
        if (not Delta)
          return false;
        auto NewOffset = checkedAdd(State.Offset, *Delta);
        if (not NewOffset)
          return false;
        Worklist.push_back({ PtrAdd.getResult(), *NewOffset, false });
        continue;
      }

      if (auto PtrSub = mlir::dyn_cast<PtrSubOp>(Op)) {
        if (PtrSub.getPointer() != State.Value)
          return false;
        auto PtrType = unwrapped_dyn_cast<PointerType>(State.Value.getType());
        uint64_t ElemSize = PtrType ?
                              getObjectSizeOrZero(PtrType.getPointeeType()) :
                              0;
        if (ElemSize == 0 or ElemSize > MaxInt64)
          return false;
        auto Step = getSignedOffset(PtrSub.getOffset());
        if (not Step)
          return false;
        auto Delta = checkedMul(*Step, static_cast<int64_t>(ElemSize));
        if (not Delta)
          return false;
        auto NewOffset = checkedSub(State.Offset, *Delta);
        if (not NewOffset)
          return false;
        Worklist.push_back({ PtrSub.getResult(), *NewOffset, false });
        continue;
      }

      if (auto Add = mlir::dyn_cast<AddOp>(Op)) {
        mlir::Value Other = Use.getOperandNumber() == 0 ? Add.getRhs() :
                                                          Add.getLhs();
        auto Delta = getSignedOffset(Other);
        if (not Delta)
          return false;
        auto NewOffset = checkedAdd(State.Offset, *Delta);
        if (not NewOffset)
          return false;
        Worklist.push_back({ Add.getResult(), *NewOffset, false });
        continue;
      }

      if (auto Sub = mlir::dyn_cast<SubOp>(Op)) {
        if (Use.getOperandNumber() != 0)
          return false;
        auto Delta = getSignedOffset(Sub.getRhs());
        if (not Delta)
          return false;
        auto NewOffset = checkedSub(State.Offset, *Delta);
        if (not NewOffset)
          return false;
        Worklist.push_back({ Sub.getResult(), *NewOffset, false });
        continue;
      }

      if (auto Subscript = mlir::dyn_cast<SubscriptOp>(Op)) {
        if (Subscript.getPointer() != State.Value)
          return false;
        uint64_t ElemSize = getObjectSizeOrZero(Subscript.getType());
        if (ElemSize == 0 or ElemSize > MaxInt64)
          return false;
        auto Index = getSignedOffset(Subscript.getIndex());
        if (not Index)
          return false;
        auto Delta = checkedMul(*Index, static_cast<int64_t>(ElemSize));
        if (not Delta)
          return false;
        auto NewOffset = checkedAdd(State.Offset, *Delta);
        if (not NewOffset)
          return false;
        Worklist.push_back({ Subscript.getResult(), *NewOffset, true });
        continue;
      }

      if (auto Indir = mlir::dyn_cast<IndirectionOp>(Op)) {
        Worklist.push_back({ Indir.getResult(), State.Offset, true });
        continue;
      }

      if (auto IndirAccess = mlir::dyn_cast<IndirectAccessOp>(Op)) {
        uint64_t FieldOffset = IndirAccess.getFieldAttr().getOffset();
        if (FieldOffset > MaxInt64)
          return false;
        auto NewOffset = checkedAdd(State.Offset,
                                    static_cast<int64_t>(FieldOffset));
        if (not NewOffset)
          return false;
        Worklist.push_back({ IndirAccess.getResult(), *NewOffset, true });
        continue;
      }

      if (auto Comma = mlir::dyn_cast<CommaOp>(Op)) {
        if (Use.getOperandNumber() == 1) {
          Worklist.push_back({ Comma.getResult(), State.Offset, false });
          continue;
        }
        if (not IsInBounds(State.Offset, 1))
          return false;
        continue;
      }

      if (auto Ternary = mlir::dyn_cast<TernaryOp>(Op)) {
        if (Use.getOperandNumber() != 0) {
          Worklist.push_back({ Ternary.getResult(), State.Offset, false });
          continue;
        }
        if (not IsInBounds(State.Offset, 1))
          return false;
        continue;
      }

      if (mlir::isa<PtrDiffOp,
                    MulOp,
                    SDivOp,
                    UDivOp,
                    SRemOp,
                    URemOp,
                    NegOp,
                    BitwiseNotOp,
                    BitwiseAndOp,
                    BitwiseOrOp,
                    BitwiseXorOp,
                    ShlOp,
                    ShrOp,
                    SarOp,
                    TruncateOp,
                    ZeroExtendOp,
                    SignExtendOp>(Op)) {
        return false;
      }

      // Escaping address (call argument, assignment RHS, comparison, return,
      // yield, etc.): verify that it stays within the string bytes.
      if (auto PtrType = unwrapped_dyn_cast<PointerType>(State.Value
                                                           .getType())) {
        mlir::Type Pointee = PtrType.getPointeeType();
        if (unwrapped_isa<IntegerType,
                          FloatType,
                          EnumType,
                          PointerType>(Pointee)) {
          uint64_t PointeeSize = getObjectSizeOrZero(Pointee);
          if (PointeeSize == 0 or not IsInBounds(State.Offset, PointeeSize))
            return false;
          continue;
        }
      }

      if (not IsInBounds(State.Offset, 1))
        return false;
    }
  }

  return true;
}

/// The text of the string held by the \p ByteCount bytes at \p Address. The
/// bytes end with the string's terminator or, for a string that has none, such
/// as a Rust string followed by the next object, with its last character.
static std::optional<std::string> readText(RawBinaryView &BinaryView,
                                           MetaAddress Address,
                                           uint64_t ByteCount) {
  auto MaybeData = BinaryView.getByAddress(Address, ByteCount);
  if (not MaybeData.has_value() or MaybeData->empty())
    return std::nullopt;

  llvm::SmallVector<uint8_t> Bytes(MaybeData->begin(), MaybeData->end());
  if (Bytes.back() != 0)
    Bytes.push_back(0);

  for (const UnicodeCStringView &Reading :
       UnicodeCStringView::getPrintable(Bytes))
    if (Reading.charSize() == 1 and Reading.data().size() == Bytes.size())
      return Reading.data().drop_back().str();

  return std::nullopt;
}

class EmitStringsPattern : public mlir::OpRewritePattern<DirectAccessOp> {
  RawBinaryView &BinaryView;

public:
  EmitStringsPattern(mlir::MLIRContext *Context, RawBinaryView &BinaryView) :
    OpRewritePattern(Context), BinaryView(BinaryView) {}

  mlir::LogicalResult
  matchAndRewrite(DirectAccessOp Access,
                  mlir::PatternRewriter &Rewriter) const override {
    auto ArrayType = mlir::dyn_cast<clift::ArrayType>(Access.getType());
    if (not ArrayType)
      return mlir::failure();

    auto CodeUnitType = mlir::dyn_cast<IntegerType>(ArrayType.getElementType());
    if (not CodeUnitType or not isConst(CodeUnitType)
        or CodeUnitType.getKind() != IntegerKind::Unsigned)
      return mlir::failure();

    uint64_t CodeUnitSize = CodeUnitType.getSize();

    // For now, only 8-bit code units are supported.
    if (CodeUnitSize != 1)
      return mlir::failure();

    MetaAddress Address = getGlobalObjectMetaAddress(Access);
    if (not Address.isValid()) {
      revng_log(Log, "Ignoring an access landing at no known address");
      return mlir::failure();
    }

    uint64_t BinarySize = CodeUnitSize * ArrayType.getElementsCount();
    if (not staysWithinString(Access, BinarySize)) {
      revng_log(Log,
                "Ignoring an access to " << Address.toString()
                                         << " that may leave the string");
      return mlir::failure();
    }

    std::optional<std::string> Content = readText(BinaryView,
                                                  Address,
                                                  BinarySize);
    if (not Content) {
      revng_log(Log, "No string at " << Address.toString() << " after all");
      return mlir::failure();
    }

    revng_log(Log, "Emitting the string at " << Address.toString());

    Rewriter.setInsertionPoint(Access);
    Rewriter.replaceOpWithNewOp<StringOp>(Access, ArrayType, *Content);

    return mlir::success();
  }
};

} // namespace

namespace revng::pypeline::piperuns {

void EmitStrings::runOnCliftFunction(const model::Function &Function,
                                     clift::FunctionOp MLIRFunction) {
  mlir::MLIRContext *Context = MLIRFunction.getContext();

  mlir::RewritePatternSet Patterns(Context);
  Patterns.add<EmitStringsPattern>(Context, BinaryView);

  // TODO: Use walkAndApplyPatterns
  if (mlir::applyPatternsAndFoldGreedily(MLIRFunction, std::move(Patterns))
        .failed())
    revng_abort("emit-strings did not converge");
}

} // namespace revng::pypeline::piperuns
