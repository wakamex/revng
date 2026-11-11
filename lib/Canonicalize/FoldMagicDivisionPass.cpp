//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <optional>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Pass.h"

#include "revng/Support/IRBuilder.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace {

/// Turn the multiplication by a reciprocal that compilers emit for a division
/// by a constant back into the division:
///
///     trunc(lshr(mul(zext(x), M), S))  ->  udiv(x, d)
///
/// where x has N bits and the multiplication is done at a wider width. The
/// rewrite is made only when it is exact for every N-bit x: d is the divisor
/// closest to 2^S / M, and 2^S <= M * d <= 2^S + 2^(S - N), the condition under
/// which the floor of x * M / 2^S equals the floor of x / d for all x < 2^N.
class FoldMagicDivisionWrapperPass : public FunctionPass {
public:
  static char ID;

public:
  FoldMagicDivisionWrapperPass() : FunctionPass(ID) {}

public:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }

public:
  bool runOnFunction(Function &F) override;
};

/// The divisor that multiplying an N-bit value by \p Multiplier and shifting
/// right by \p Shift divides it by, if that is exact for every N-bit value
std::optional<APInt>
divisorOf(const APInt &Multiplier, unsigned Shift, unsigned N) {
  if (Shift < N or Multiplier.isZero())
    return std::nullopt;

  // Work wide enough for 2^Shift and for the product of the multiplier and a
  // divisor of up to N bits
  unsigned Width = std::max(Shift + 2, Multiplier.getBitWidth() + N + 1);
  APInt M = Multiplier.zext(Width);
  APInt Power = APInt::getOneBitSet(Width, Shift);

  APInt Divisor = (Power + M.lshr(1)).udiv(M);
  if (Divisor.ult(2) or Divisor.getActiveBits() > N)
    return std::nullopt;

  APInt Product = M * Divisor;
  if (Product.ult(Power))
    return std::nullopt;

  APInt Error = Product - Power;
  if (Error.ugt(APInt::getOneBitSet(Width, Shift - N)))
    return std::nullopt;

  return Divisor.trunc(N);
}

bool foldMagicDivisions(Function &F) {
  SmallVector<std::pair<TruncInst *, Value *>, 8> Candidates;
  SmallVector<APInt, 8> Divisors;

  for (Instruction &I : instructions(F)) {
    auto *Truncation = dyn_cast<TruncInst>(&I);
    if (Truncation == nullptr)
      continue;

    Value *X = nullptr;
    const APInt *Multiplier = nullptr;
    const APInt *Shift = nullptr;
    if (not match(Truncation->getOperand(0),
                  m_LShr(m_c_Mul(m_ZExt(m_Value(X)), m_APInt(Multiplier)),
                         m_APInt(Shift))))
      continue;

    if (X->getType() != Truncation->getType())
      continue;

    unsigned N = X->getType()->getIntegerBitWidth();
    if (Shift->uge(Multiplier->getBitWidth()))
      continue;

    auto Divisor = divisorOf(*Multiplier, Shift->getZExtValue(), N);
    if (not Divisor)
      continue;

    Candidates.emplace_back(Truncation, X);
    Divisors.push_back(*Divisor);
  }

  for (auto &&[Pair, Divisor] : llvm::zip(Candidates, Divisors)) {
    auto &&[Truncation, X] = Pair;
    revng::IRBuilder Builder(Truncation);
    Value *Quotient = Builder.CreateUDiv(X,
                                         ConstantInt::get(X->getType(),
                                                          Divisor));
    Truncation->replaceAllUsesWith(Quotient);
    Truncation->eraseFromParent();
  }

  return not Candidates.empty();
}

} // namespace

char FoldMagicDivisionWrapperPass::ID = 0;
using Register = RegisterPass<FoldMagicDivisionWrapperPass>;
static Register X("fold-magic-division",
                  "Turn multiplications by a reciprocal back into divisions",
                  false,
                  false);

bool FoldMagicDivisionWrapperPass::runOnFunction(Function &F) {
  return foldMagicDivisions(F);
}
