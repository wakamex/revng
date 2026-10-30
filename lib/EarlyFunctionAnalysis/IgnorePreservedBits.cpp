/// Leave the merge a sub-register write performs out of the register usage
/// analysis.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

#include "revng/EarlyFunctionAnalysis/AnalyzeRegisterUsage.h"
#include "revng/EarlyFunctionAnalysis/IgnorePreservedBits.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;
using namespace llvm::PatternMatch;

static Logger Log("ignore-preserved-bits");

/// Matches `store (or (and (load @CSV), HighMask), Low), @CSV`.
static LoadInst *matchPreservingWrite(StoreInst *Store, const DataLayout &DL) {
  auto *CSV = dyn_cast<GlobalVariable>(skipCasts(Store->getPointerOperand()));
  if (CSV == nullptr)
    return nullptr;

  Value *Preserved = nullptr;
  Value *Written = nullptr;
  const APInt *HighMask = nullptr;
  if (not match(Store->getValueOperand(),
                m_c_Or(m_c_And(m_Value(Preserved), m_APInt(HighMask)),
                       m_Value(Written))))
    return nullptr;

  // What survives has to be the top of the register, and something has to
  // survive: the complement of the mask is `(1 << N) - 1`, with `N` below the
  // width of the register.
  APInt LowMask = ~*HighMask;
  if (not LowMask.isMask() or LowMask.isAllOnes())
    return nullptr;

  // What is merged in has to fit in that field. If it does not, the `and` is
  // doing something other than making room and we do not know what.
  if (not MaskedValueIsZero(Written, *HighMask, DL))
    return nullptr;

  // The preserved bits have to come from the very register being written.
  auto *Load = dyn_cast<LoadInst>(Preserved);
  if (Load == nullptr)
    return nullptr;

  if (skipCasts(Load->getPointerOperand()) != CSV)
    return nullptr;

  // Passing over the read is only invisible if this merge is its sole
  // consumer.
  if (not Load->hasOneUse())
    return nullptr;

  return Load;
}

/// \return whether \p A and \p B compute the same value: the same instruction
///         applied to operands that compute the same value, touching no
///         memory, down to a common value.
static bool computeTheSame(Value *A, Value *B, unsigned Depth = 4) {
  if (A == B)
    return true;

  auto *IA = dyn_cast<Instruction>(A);
  auto *IB = dyn_cast<Instruction>(B);
  if (Depth == 0 or IA == nullptr or IB == nullptr
      or not IA->isSameOperationAs(IB) or IA->mayReadOrWriteMemory()
      or isa<PHINode>(IA))
    return false;

  for (auto [OA, OB] : llvm::zip(IA->operands(), IB->operands()))
    if (not computeTheSame(OA, OB, Depth - 1))
      return false;

  return true;
}

/// Replace a comparison of a value with itself by its outcome.
///
/// An all-ones idiom such as `pcmpeqd %xmm0, %xmm0` compares each lane of a
/// register with itself: its result does not depend on the register, but each
/// lane is extracted twice from the value the register held, which makes the
/// register look read. Lanes extracted for nothing else are deleted.
static bool foldSelfComparisons(Function &F) {
  SmallVector<ICmpInst *, 8> Comparisons;
  for (Instruction &I : instructions(F))
    if (auto *Compare = dyn_cast<ICmpInst>(&I))
      if (computeTheSame(Compare->getOperand(0), Compare->getOperand(1)))
        Comparisons.push_back(Compare);

  for (ICmpInst *Compare : Comparisons) {
    bool Outcome = ICmpInst::isTrueWhenEqual(Compare->getPredicate());
    WeakTrackingVH LHS = Compare->getOperand(0);
    WeakTrackingVH RHS = Compare->getOperand(1);
    Compare->replaceAllUsesWith(ConstantInt::getBool(Compare->getType(),
                                                     Outcome));
    Compare->eraseFromParent();
    if (LHS)
      RecursivelyDeleteTriviallyDeadInstructions(LHS);
    if (RHS)
      RecursivelyDeleteTriviallyDeadInstructions(RHS);
  }

  return not Comparisons.empty();
}

llvm::PreservedAnalyses
IgnorePreservedBitsPass::run(llvm::Function &F,
                             llvm::FunctionAnalysisManager &) {
  const DataLayout &DL = F.getParent()->getDataLayout();

  bool Changed = foldSelfComparisons(F);

  SmallVector<LoadInst *, 8> Reads;
  for (Instruction &I : instructions(F))
    if (auto *Store = dyn_cast<StoreInst>(&I))
      if (LoadInst *Load = matchPreservingWrite(Store, DL))
        Reads.push_back(Load);

  for (LoadInst *Read : Reads) {
    auto *CSV = cast<GlobalVariable>(skipCasts(Read->getPointerOperand()));
    revng_log(Log,
              "Ignoring the read-back of the bits preserved across a write to "
                << CSV->getName().str() << " in " << F.getName().str());

    efa::ignoreInRegisterUsage(*Read);
  }

  revng_log(Log,
            "Ignored " << Reads.size() << " read(s) in " << F.getName().str());

  // Otherwise only metadata was added, so nothing an analysis computed is
  // stale.
  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
