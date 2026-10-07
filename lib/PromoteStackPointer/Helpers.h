#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Model/FunctionTags.h"
#include "revng/Ranks/IRHelpers.h"
#include "revng/Support/MetaAddress.h"

inline MetaAddress getCallerBlockAddress(llvm::Instruction *I) {
  auto MaybeLocation = getLocation(I);
  revng_assert(MaybeLocation);
  return MaybeLocation->parent().back().start();
}

/// The call a stack-size marker belongs to, or nullptr if the optimizer
/// deleted it, for example because it called through an undefined register.
/// The call is in the marker's caller block: the first call found past it
/// that belongs to another caller block is someone else's.
inline llvm::CallInst *findAssociatedCall(llvm::CallInst *SSACSCall) {
  using namespace llvm;

  // Look for the actual call in the same block or the next one
  Instruction *I = SSACSCall->getNextNode();
  while (I != SSACSCall) {
    if (auto *Call = getCallToIsolatedFunction(I)) {
      MetaAddress SSACSBlockAddress = getCallerBlockAddress(SSACSCall);
      if (getCallerBlockAddress(I) != SSACSBlockAddress)
        return nullptr;
      return Call;
    } else if (I->isTerminator()) {
      if (I->getNumSuccessors() != 1)
        return nullptr;
      I = I->getSuccessor(0)->getFirstNonPHI();
    } else {
      I = I->getNextNode();
    }
  }

  return nullptr;
}
