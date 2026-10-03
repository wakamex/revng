#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <optional>
#include <set>
#include <string>

#include "revng/EarlyFunctionAnalysis/FunctionSummaryOracle.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/MetaAddress.h"

namespace model {
class Binary;
} // namespace model

namespace efa {

/// The part of a call site's description that the preliminary analysis of the
/// caller looks at: whether the call returns or gets inlined, the registers it
/// clobbers, the final stack offset and whether it is a tail call.
struct CallSiteView {
  bool NoReturn = false;
  bool HasOneBrokenReturn = false;
  CSVSet ClobberedRegisters;
  std::optional<int64_t> ElectedFSO;
  bool IsTailCall = false;

  static CallSiteView of(const FunctionSummary &Summary, bool IsTailCall);

  bool operator==(const CallSiteView &Other) const = default;
  bool operator<(const CallSiteView &Other) const;
};

/// What one preliminary analysis of a function (`CFGAnalyzer::analyze`) read
/// from the oracle and the model, with the answers it got.
///
/// The analysis is a function of the lifted code of `root` and of these
/// answers: if every query recorded here gets the same answer again, the
/// analysis asks the same queries and computes the same summary.
class PreliminaryAnalysisInputs {
public:
  struct CallSiteQuery {
    MetaAddress Caller;
    BasicBlockID Block;
    MetaAddress Callee;
    std::string Symbol;

    auto operator<=>(const CallSiteQuery &Other) const = default;
  };

private:
  /// Answers are shared among all the analyses of a cache, since most call
  /// sites of a program get one of a few answers
  std::set<CallSiteView> *Views = nullptr;

  std::map<CallSiteQuery, const CallSiteView *> CallSites;
  std::map<MetaAddress, bool> LocalNoReturn;
  std::map<std::string, bool> DynamicNoReturn;
  std::map<MetaAddress, bool> IsFunction;

  /// The return blocks of the CFG the oracle held for the analyzed function
  /// itself, if it held one
  std::optional<std::set<MetaAddress>> OwnReturnBlocks;

public:
  explicit PreliminaryAnalysisInputs(std::set<CallSiteView> &Views) :
    Views(&Views) {}

public:
  void recordCallSite(CallSiteQuery &&Query,
                      const FunctionSummary &Summary,
                      bool IsTailCall);
  void recordLocalNoReturn(const MetaAddress &Function, bool NoReturn);
  void recordDynamicNoReturn(llvm::StringRef Name, bool NoReturn);
  void recordIsFunction(const MetaAddress &Address, bool IsFunction);
  void recordOwnCFG(const SortedVector<efa::BasicBlock> &CFG);

public:
  /// \return whether every query recorded here gets the same answer from
  ///         \p Oracle and \p Binary for the analysis of \p Entry
  bool stillHold(const MetaAddress &Entry,
                 FunctionSummaryOracle &Oracle,
                 const model::Binary &Binary) const;

  size_t size() const {
    return CallSites.size() + LocalNoReturn.size() + DynamicNoReturn.size()
           + IsFunction.size() + 1;
  }
};

/// The latest preliminary analysis of each function, with what it read, kept
/// across the runs of DetectABI over one module so that an analysis whose
/// inputs did not change is not repeated.
class PreliminaryAnalysisCache {
private:
  struct Analysis {
    PreliminaryAnalysisInputs Inputs;
    FunctionSummary Result;
  };

private:
  std::set<CallSiteView> Views;
  std::map<MetaAddress, Analysis> Analyses;

public:
  PreliminaryAnalysisInputs newInputs() {
    return PreliminaryAnalysisInputs(Views);
  }

  void record(const MetaAddress &Entry,
              PreliminaryAnalysisInputs &&Inputs,
              const FunctionSummary &Result);

  /// \return the result of the latest analysis of \p Entry, if every query it
  ///         made still gets the same answer
  std::optional<FunctionSummary> reusable(const MetaAddress &Entry,
                                          FunctionSummaryOracle &Oracle,
                                          const model::Binary &Binary) const;
};

} // namespace efa
