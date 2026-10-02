#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Model/Binary.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/Debug.h"
#include "revng/Support/MetaAddress.h"

namespace efa {

/// Instructions carrying this are left out of the register usage analysis, as
/// if they touched no register at all.
constexpr const char *IgnoreMDName = "revng.register-usage.ignore";

/// Leave \p I out of the register usage analysis.
inline void ignoreInRegisterUsage(llvm::Instruction &I) {
  I.setMetadata(IgnoreMDName, llvm::MDNode::get(I.getContext(), {}));
}

/// \return whether \p I was left out of the register usage analysis.
inline bool isIgnoredInRegisterUsage(const llvm::Instruction &I) {
  return I.getMetadata(IgnoreMDName) != nullptr;
}

/// Order CSVs by name: the analyses emit code for each CSV in a set in
/// iteration order, and the result of optimizing that code depends on it, so
/// an order based on addresses makes the results depend on memory layout
struct CSVByName {
  bool operator()(const llvm::GlobalVariable *LHS,
                  const llvm::GlobalVariable *RHS) const {
    // Lookups may ask for the CSV of a register that has none
    if (LHS == nullptr or RHS == nullptr)
      return LHS == nullptr and RHS != nullptr;
    return LHS->getName() < RHS->getName();
  }
};

using CSVSet = std::set<llvm::GlobalVariable *, CSVByName>;

class RUAResults {
public:
  struct CallSiteResults {
    MetaAddress CalleeAddress;
    CSVSet ArgumentsRegisters;
    CSVSet ReturnValuesRegisters;

    /// Registers that hold, at the call, the value they had at the entry of the
    /// caller on every path, with no other call in between. Only the analysis
    /// of the caller produces it, so `combine` does not carry it over.
    CSVSet IncomingRegisters;

    bool operator==(const CallSiteResults &Other) const = default;
  };

public:
  // Per function analysis
  CSVSet ArgumentsRegisters;
  CSVSet ReturnValuesRegisters;

  /// The return values of this function that a caller reads after a call to it
  CSVSet ReadReturnValuesRegisters;

  // Per call site analysis
  std::map<BasicBlockID, CallSiteResults> CallSites;

public:
  bool operator==(const RUAResults &Other) const = default;

public:
  void combine(const RUAResults &Other) {
    auto HandleMap = [](CSVSet &ThisSet, const CSVSet &OtherSet) {
      for (auto *CSV : OtherSet)
        ThisSet.insert(CSV);
    };

    HandleMap(ArgumentsRegisters, Other.ArgumentsRegisters);
    HandleMap(ReturnValuesRegisters, Other.ReturnValuesRegisters);
    HandleMap(ReadReturnValuesRegisters, Other.ReadReturnValuesRegisters);

    for (auto &[OtherBlockID, OtherResults] : Other.CallSites) {
      HandleMap(CallSites[OtherBlockID].ArgumentsRegisters,
                OtherResults.ArgumentsRegisters);
      HandleMap(CallSites[OtherBlockID].ReturnValuesRegisters,
                OtherResults.ReturnValuesRegisters);
    }
  }

public:
  // Debug methods
  void dump() const debug_function { dump(dbg, ""); }

  template<typename T>
  void dump(T &Output, const char *Prefix = "") const;
};

extern template void RUAResults::dump<Logger>(Logger &, const char *) const;

/// \param ReturnValues the registers the callers of \p F read after it returns
RUAResults analyzeRegisterUsage(llvm::Function *F,
                                model::Architecture::Values Architecture,
                                llvm::Function *,
                                llvm::Function *,
                                llvm::Function *,
                                const CSVSet &ReturnValues);

} // namespace efa
