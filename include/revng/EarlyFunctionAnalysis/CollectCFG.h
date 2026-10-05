#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/BasicAnalyses/CSVGlobals.h"
#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/CFGAnalyzer.h"
#include "revng/EarlyFunctionAnalysis/FunctionBundle.h"
#include "revng/Pipebox/TupleTreeContainer.h"
#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/LLVMContainer.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline {

class CFGMap : public TupleTreeContainer<efa::FunctionBundle, Kinds::Function> {
public:
  static constexpr llvm::StringRef Name = "CFGMap";
  static constexpr llvm::StringRef Compression = "zstd;level=1";
};

namespace piperuns {

class CollectCFG {
private:
  const class Model &Model;
  CFGMap &Output;

  RootFunction Root;
  CSVGlobals Globals;
  efa::FunctionSummaryOracle Oracle;
  efa::CFGAnalyzer Analyzer;

  /// Held from the first `prepare` on: the optimization pipelines on other
  /// threads read the scan limits it sets
  std::optional<efa::UnboundedScanLimits> ParallelLimits;

public:
  static constexpr llvm::StringRef Name = "collect-cfg";
  using Arguments = TypeList<PipeRunArgument<LLVMRootContainer,
                                             "Input",
                                             "LLVM module to analyze to "
                                             "produce the CFG",
                                             // The root container is
                                             // manipulated to create the
                                             // CFGMap, hence the need to
                                             // declare Access::Read and a
                                             // non-const argument.
                                             Access::Read>,
                             PipeRunArgument<CFGMap,
                                             "Output",
                                             "The produced CFG for each "
                                             "function",
                                             Access::Write>>;

  CollectCFG(const class Model &Model,
             llvm::StringRef Config,
             llvm::StringRef DynamicConfig,
             LLVMRootContainer &Input,
             CFGMap &Output);
  void runOnFunction(const model::Function &TheFunction);

  // The same analysis with its optimization pipeline on another thread
  using Ticket = efa::ShippedAnalysis;
  using Result = std::vector<efa::IndirectBranchFacts>;
  Ticket prepare(const model::Function &TheFunction);
  static Result work(const Ticket &Shipped);
  void finish(const model::Function &TheFunction, Ticket &&Shipped, Result &&Facts);
  static size_t cost(const Ticket &Shipped) { return Shipped.Instructions; }
  bool canRunInParallel() const { return efa::CFGAnalyzer::canRunInParallel(); }

private:
  /// Store the control-flow graph \p Blocks of the function at \p Entry,
  /// with the `AlwaysInline` functions it reaches
  void complete(const MetaAddress &Entry,
                SortedVector<efa::BasicBlock> &&Blocks);

  efa::ControlFlowGraph analyzeFunction(const MetaAddress &Entry);

  /// Record in \p Bundle the control-flow graph of each `AlwaysInline`
  /// function reachable from its main function through `AlwaysInline` calls
  void collectAlwaysInlineFunctions(efa::FunctionBundle &Bundle);
};

} // namespace piperuns

} // namespace revng::pypeline
