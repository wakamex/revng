#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"

#include "revng/MFP/MFP.h"
#include "revng/RegisterUsageAnalyses/Function.h"

namespace rua {

/// Forward analysis of the registers that hold the value they had at the entry
/// of the function on every path: a register leaves the set at a write or a
/// clobber, and every register leaves it at a call, since the ABI does not keep
/// argument registers across calls. The lattice is ordered by inclusion in
/// reverse, so its bottom is the set of all registers and paths meet by
/// intersection.
class IncomingValues {
private:
  using Set = llvm::BitVector;

public:
  using LatticeElement = Set;
  using GraphType = Function *;
  using Label = BlockNode *;

private:
  Set All;
  const llvm::SmallPtrSetImpl<const BlockNode *> &Calls;

public:
  /// \param Calls the nodes that are calls
  IncomingValues(const Function &F,
                 const llvm::SmallPtrSetImpl<const BlockNode *> &Calls) :
    All(F.csvCount(), true), Calls(Calls) {}

public:
  Set defaultValue() const { return All; }

public:
  Set combineValues(const Set &LHS, const Set &RHS) const {
    Set Result = LHS;
    Result &= RHS;
    return Result;
  }

  bool isLessOrEqual(const Set &LHS, const Set &RHS) const {
    return not RHS.test(LHS);
  }

  Set applyTransferFunction(const BlockNode *Block,
                            const Set &InitialState,
                            mfp::NoExtraState &) const {
    if (Calls.contains(Block))
      return Set(All.size());

    Set Result = InitialState;

    for (const Operation &Operation : *Block) {
      switch (Operation.Type) {
      case OperationType::Read:
        break;

      case OperationType::Write:
      case OperationType::Clobber:
        Result.reset(Operation.Target);
        break;

      case OperationType::Invalid:
        revng_abort();
        break;
      }
    }

    return Result;
  }
};

static_assert(mfp::MonotoneFrameworkInstance<IncomingValues>);

} // namespace rua
