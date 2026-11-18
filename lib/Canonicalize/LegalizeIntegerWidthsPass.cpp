//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include "revng/Support/Assert.h"

using namespace llvm;

/// Loop optimizations leave integers of widths that C has no type for, such as
/// the i5 of `x & 31`. This pass computes them in the next wider integer C has:
/// the low bits of additions, multiplications and left shifts do not depend on
/// the bits above them, and where a value is observed whole, as in a comparison
/// or an extension, the bits above its width are cleared or sign extended.
class LegalizeIntegerWidthsPass : public FunctionPass {
public:
  static char ID;

public:
  LegalizeIntegerWidthsPass() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override;
};

char LegalizeIntegerWidthsPass::ID = 0;
using Register = RegisterPass<LegalizeIntegerWidthsPass>;
static Register X("legalize-integer-widths",
                  "Compute integers of widths C lacks in wider ones",
                  false,
                  false);

static bool isNarrow(Type *T) {
  auto *Integer = dyn_cast<IntegerType>(T);
  if (Integer == nullptr)
    return false;
  unsigned Width = Integer->getBitWidth();
  return Width < 128 and Width != 1 and Width != 8 and Width != 16
         and Width != 32 and Width != 64;
}

static IntegerType *wideType(Type *T) {
  unsigned Width = T->getIntegerBitWidth();
  for (unsigned Candidate : { 8U, 16U, 32U, 64U, 128U })
    if (Width <= Candidate)
      return IntegerType::get(T->getContext(), Candidate);
  revng_abort();
}

static bool involvesNarrow(const Instruction &I) {
  if (isNarrow(I.getType()))
    return true;
  for (const Use &Operand : I.operands())
    if (isNarrow(Operand->getType()))
      return true;
  return false;
}

static bool isSupported(const Instruction &I) {
  for (const Use &Operand : I.operands())
    if (isNarrow(Operand->getType())
        and not isa<Instruction, ConstantInt, UndefValue>(Operand.get()))
      return false;

  if (isa<ZExtInst, SExtInst, TruncInst, ICmpInst>(I))
    return true;
  return isNarrow(I.getType())
         and isa<BinaryOperator, PHINode, SelectInst, FreezeInst>(I);
}

namespace {

class Widener {
  DenseMap<Value *, Value *> Wide;
  IRBuilder<> Builder;

public:
  explicit Widener(LLVMContext &Context) : Builder(Context) {}

  void addPHI(PHINode &Phi) {
    Builder.SetInsertPoint(&Phi);
    Wide[&Phi] = Builder.CreatePHI(wideType(Phi.getType()),
                                   Phi.getNumIncomingValues());
  }

  void fillPHI(PHINode &Phi) {
    auto *New = cast<PHINode>(Wide.lookup(&Phi));
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      New->addIncoming(wide(Phi.getIncomingValue(I)), Phi.getIncomingBlock(I));
  }

  void rewrite(Instruction &I) {
    Builder.SetInsertPoint(&I);
    Value *New = compute(I);
    if (isNarrow(I.getType()))
      Wide[&I] = New;
    else
      I.replaceAllUsesWith(New);
  }

private:
  Value *wide(Value *V) {
    IntegerType *Type = wideType(V->getType());
    if (auto *Constant = dyn_cast<ConstantInt>(V))
      return ConstantInt::get(Type, Constant->getZExtValue());
    if (isa<PoisonValue>(V))
      return PoisonValue::get(Type);
    if (isa<UndefValue>(V))
      return UndefValue::get(Type);
    Value *Result = Wide.lookup(V);
    revng_assert(Result != nullptr);
    return Result;
  }

  /// \p V with the bits above its width cleared
  Value *zeroExtended(Value *V) {
    if (not isNarrow(V->getType()))
      return V;
    unsigned Width = V->getType()->getIntegerBitWidth();
    return Builder.CreateAnd(wide(V), maskTrailingOnes<uint64_t>(Width));
  }

  /// \p V with the bits above its width copied from its sign
  Value *signExtended(Value *V) {
    if (not isNarrow(V->getType()))
      return V;
    Value *Result = wide(V);
    unsigned Shift = Result->getType()->getIntegerBitWidth()
                     - V->getType()->getIntegerBitWidth();
    return Builder.CreateAShr(Builder.CreateShl(Result, Shift), Shift);
  }

  Value *compute(Instruction &I) {
    Type *Result = isNarrow(I.getType()) ? wideType(I.getType()) : I.getType();

    if (auto *Compare = dyn_cast<ICmpInst>(&I)) {
      auto Extend = [&](Value *V) {
        return Compare->isSigned() ? signExtended(V) : zeroExtended(V);
      };
      Value *LHS = Extend(Compare->getOperand(0));
      Value *RHS = Extend(Compare->getOperand(1));
      return Builder.CreateICmp(Compare->getPredicate(), LHS, RHS);
    }

    if (isa<ZExtInst>(I))
      return Builder.CreateZExtOrTrunc(zeroExtended(I.getOperand(0)), Result);

    if (isa<SExtInst>(I))
      return Builder.CreateSExtOrTrunc(signExtended(I.getOperand(0)), Result);

    if (isa<TruncInst>(I)) {
      Value *Operand = I.getOperand(0);
      if (isNarrow(Operand->getType()))
        Operand = wide(Operand);
      return Builder.CreateZExtOrTrunc(Operand, Result);
    }

    if (auto *Select = dyn_cast<SelectInst>(&I))
      return Builder.CreateSelect(Select->getCondition(),
                                  wide(Select->getTrueValue()),
                                  wide(Select->getFalseValue()));

    if (isa<FreezeInst>(I))
      return Builder.CreateFreeze(wide(I.getOperand(0)));

    auto *Binary = cast<BinaryOperator>(&I);
    Value *LHS = Binary->getOperand(0);
    Value *RHS = Binary->getOperand(1);
    switch (Binary->getOpcode()) {
    case Instruction::LShr:
    case Instruction::UDiv:
    case Instruction::URem:
      LHS = zeroExtended(LHS);
      RHS = zeroExtended(RHS);
      break;

    case Instruction::AShr:
      LHS = signExtended(LHS);
      RHS = zeroExtended(RHS);
      break;

    case Instruction::SDiv:
    case Instruction::SRem:
      LHS = signExtended(LHS);
      RHS = signExtended(RHS);
      break;

    default:
      LHS = wide(LHS);
      RHS = wide(RHS);
      break;
    }
    return Builder.CreateBinOp(Binary->getOpcode(), LHS, RHS);
  }
};

} // namespace

bool LegalizeIntegerWidthsPass::runOnFunction(Function &F) {
  // Definitions come before their uses in reverse post order, except for the
  // incoming values of phis
  SmallVector<Instruction *, 16> ToRewrite;
  DenseSet<const BasicBlock *> Reachable;
  for (BasicBlock *BB : ReversePostOrderTraversal<Function *>(&F)) {
    Reachable.insert(BB);
    for (Instruction &I : *BB)
      if (involvesNarrow(I))
        ToRewrite.push_back(&I);
  }

  if (ToRewrite.empty())
    return false;

  // Leave alone a function this pass cannot rewrite whole
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (involvesNarrow(I)
          and (not Reachable.contains(&BB) or not isSupported(I)))
        return false;

  Widener Rewriter(F.getContext());
  for (Instruction *I : ToRewrite)
    if (auto *Phi = dyn_cast<PHINode>(I))
      Rewriter.addPHI(*Phi);

  for (Instruction *I : ToRewrite)
    if (not isa<PHINode>(I))
      Rewriter.rewrite(*I);

  for (Instruction *I : ToRewrite)
    if (auto *Phi = dyn_cast<PHINode>(I))
      Rewriter.fillPHI(*Phi);

  for (Instruction *I : ToRewrite)
    I->dropAllReferences();
  for (Instruction *I : ToRewrite) {
    I->replaceAllUsesWith(PoisonValue::get(I->getType()));
    I->eraseFromParent();
  }

  return true;
}
