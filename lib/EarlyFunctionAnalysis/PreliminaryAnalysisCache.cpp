//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <tuple>

#include "revng/EarlyFunctionAnalysis/PreliminaryAnalysisCache.h"
#include "revng/Model/Binary.h"

namespace efa {

CallSiteView CallSiteView::of(const FunctionSummary &Summary,
                              bool IsTailCall) {
  namespace FA = model::FunctionAttribute;
  return CallSiteView{
    .NoReturn = Summary.Attributes.contains(FA::NoReturn),
    .HasOneBrokenReturn = Summary.Attributes.contains(FA::HasOneBrokenReturn),
    .ClobberedRegisters = Summary.ClobberedRegisters,
    .ElectedFSO = Summary.ElectedFSO,
    .IsTailCall = IsTailCall,
  };
}

bool CallSiteView::operator<(const CallSiteView &Other) const {
  // Only used to share equal answers, so any strict order consistent with
  // equality will do
  auto Clobbered = [](const CSVSet &Set) {
    return std::vector<llvm::GlobalVariable *>(Set.begin(), Set.end());
  };
  return std::tuple(NoReturn,
                    HasOneBrokenReturn,
                    ElectedFSO,
                    IsTailCall,
                    Clobbered(ClobberedRegisters))
         < std::tuple(Other.NoReturn,
                      Other.HasOneBrokenReturn,
                      Other.ElectedFSO,
                      Other.IsTailCall,
                      Clobbered(Other.ClobberedRegisters));
}

void PreliminaryAnalysisInputs::recordCallSite(CallSiteQuery &&Query,
                                               const FunctionSummary &Summary,
                                               bool IsTailCall) {
  const CallSiteView *View = &*Views->insert(CallSiteView::of(Summary,
                                                              IsTailCall))
                                 .first;
  CallSites.emplace(std::move(Query), View);
}

void PreliminaryAnalysisInputs::recordLocalNoReturn(const MetaAddress &Function,
                                                    bool NoReturn) {
  LocalNoReturn.emplace(Function, NoReturn);
}

void PreliminaryAnalysisInputs::recordDynamicNoReturn(llvm::StringRef Name,
                                                      bool NoReturn) {
  DynamicNoReturn.emplace(Name.str(), NoReturn);
}

void PreliminaryAnalysisInputs::recordIsFunction(const MetaAddress &Address,
                                                 bool IsFunction) {
  this->IsFunction.emplace(Address, IsFunction);
}

static std::optional<std::set<MetaAddress>>
returnBlocksOf(const SortedVector<efa::BasicBlock> &CFG) {
  if (CFG.empty())
    return std::nullopt;

  std::set<MetaAddress> Result;
  for (const efa::BasicBlock &Block : CFG)
    for (const auto &Successor : Block.Successors())
      if (Successor->Type() == efa::FunctionEdgeType::Return)
        Result.insert(Block.ID().start());
  return Result;
}

void PreliminaryAnalysisInputs::recordOwnCFG(const SortedVector<efa::BasicBlock>
                                               &CFG) {
  OwnReturnBlocks = returnBlocksOf(CFG);
}

bool PreliminaryAnalysisInputs::stillHold(const MetaAddress &Entry,
                                          FunctionSummaryOracle &Oracle,
                                          const model::Binary &Binary) const {
  // Cheapest first: the model, then the oracle
  for (const auto &[Address, Recorded] : IsFunction)
    if (Binary.Functions().contains(Address) != Recorded)
      return false;

  using namespace model::FunctionAttribute;
  for (const auto &[Name, Recorded] : DynamicNoReturn) {
    const auto *Dynamic = Binary.ImportedDynamicFunctions().tryGet(Name);
    if (Dynamic == nullptr
        or Dynamic->Attributes().contains(NoReturn) != Recorded)
      return false;
  }

  if (returnBlocksOf(Oracle.getLocalFunction(Entry).CFG) != OwnReturnBlocks)
    return false;

  // Functions are only ever added to the model, but asking the oracle about
  // one that is not there would fail rather than answer differently
  for (const auto &[Function, Recorded] : LocalNoReturn)
    if (not Binary.Functions().contains(Function)
        or Oracle.getLocalFunction(Function).Attributes.contains(NoReturn)
             != Recorded)
      return false;

  for (const auto &[Query, Recorded] : CallSites) {
    if (not Binary.Functions().contains(Query.Caller))
      return false;
    auto &&[Summary, IsTailCall] = Oracle.getCallSite(Query.Caller,
                                                      Query.Block,
                                                      Query.Callee,
                                                      Query.Symbol);
    if (CallSiteView::of(*Summary, IsTailCall) != *Recorded)
      return false;
  }

  return true;
}

void PreliminaryAnalysisCache::record(const MetaAddress &Entry,
                                      PreliminaryAnalysisInputs &&Inputs,
                                      const FunctionSummary &Result) {
  Analysis New{ std::move(Inputs), Result.clone() };
  auto It = Analyses.find(Entry);
  if (It == Analyses.end())
    Analyses.emplace(Entry, std::move(New));
  else
    It->second = std::move(New);
}

std::optional<FunctionSummary>
PreliminaryAnalysisCache::reusable(const MetaAddress &Entry,
                                   FunctionSummaryOracle &Oracle,
                                   const model::Binary &Binary) const {
  auto It = Analyses.find(Entry);
  if (It == Analyses.end())
    return std::nullopt;

  const Analysis &Previous = It->second;
  if (not Previous.Inputs.stillHold(Entry, Oracle, Binary))
    return std::nullopt;

  return Previous.Result.clone();
}

} // namespace efa
