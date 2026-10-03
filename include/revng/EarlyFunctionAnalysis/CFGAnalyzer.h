#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"

#include "revng/EarlyFunctionAnalysis/CallHandler.h"
#include "revng/EarlyFunctionAnalysis/Outliner.h"
#include "revng/EarlyFunctionAnalysis/TemporaryOpaqueFunction.h"
#include "revng/Model/Binary.h"
#include "revng/Model/ProgramCounterHandler.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/OpaqueRegisterUser.h"
#include "revng/Support/TemporaryLLVMOption.h"

class FunctionSummaryOracle;
class PreliminaryAnalysisInputs;

namespace llvm {
class Task;
class raw_ostream;
class Module;
class GlobalVariable;
class Function;
} // namespace llvm

namespace efa {

class CallSummarizer : public CallHandler {
private:
  llvm::Module *M = nullptr;
  llvm::Function *PreCallHook = nullptr;
  llvm::Function *PostCallHook = nullptr;
  llvm::Function *RetHook = nullptr;
  llvm::GlobalVariable *SPCSV = nullptr;
  llvm::SmallSet<MetaAddress, 4> *ReturnBlocks = nullptr;
  OpaqueRegisterUser Clobberer;

public:
  CallSummarizer(llvm::Module *M,
                 llvm::Function *PreCallHook,
                 llvm::Function *PostCallHook,
                 llvm::Function *RetHook,
                 llvm::GlobalVariable *SPCSV,
                 llvm::SmallSet<MetaAddress, 4> *ReturnBlocks);

public:
  void handleCall(MetaAddress CallerBlock,
                  revng::IRBuilder &Builder,
                  MetaAddress Callee,
                  const CSVSet &ClobberedRegisters,
                  const std::optional<int64_t> &MaybeFSO,
                  bool IsNoReturn,
                  bool IsTailCall,
                  llvm::Value *SymbolNamePointer) final;

  void handlePostNoReturn(revng::IRBuilder &Builder,
                          const llvm::DebugLoc &DbgLocation) final;

  void handleIndirectJump(revng::IRBuilder &Builder,
                          MetaAddress Block,
                          const CSVSet &ClobberedRegisters,
                          llvm::Value *SymbolNamePointer) final;
};

/// Lifts the scan limits of MemorySSA and of memory dependence analysis for as
/// long as it lives, which the optimization pipeline of `CFGAnalyzer::analyze`
/// needs. Hold one around a whole loop of analyses: setting a process-wide
/// option around each pipeline run would race with the pipelines of other
/// threads.
class UnboundedScanLimits {
private:
  TemporaryLLVMOption<unsigned> MemorySSALimit;
  TemporaryLLVMOption<unsigned> MemoryDependenceLimit;
  static inline unsigned Active = 0;

public:
  UnboundedScanLimits();
  ~UnboundedScanLimits() { --Active; }

  UnboundedScanLimits(const UnboundedScanLimits &) = delete;
  UnboundedScanLimits &operator=(const UnboundedScanLimits &) = delete;

public:
  static bool active() { return Active != 0; }
};

/// The instructions each CFG block decodes, as (block, instruction) pairs
using DecodedInstructions = std::set<std::pair<BasicBlockID, BasicBlockID>>;

/// What the optimized `indirect_branch_info` call of one indirect branch tells:
/// the constant arguments the preliminary analysis reads
struct IndirectBranchFacts {
  BasicBlockID Block;
  std::string CalledSymbol;
  bool JumpsToReturnAddress = false;
  std::optional<int64_t> StackPointerOffset;
  bool ReturnAddressPreserved = false;

  /// For each register of `CFGAnalyzer::abiCSVs`, whether it is known to hold
  /// its value at the function's entry
  llvm::SmallVector<bool, 16> PreservedRegisters;
};

/// A function outlined and prepared for the optimization pipeline, with what
/// the analysis recovered from it before the pipeline
struct PreparedFunction {
  OutlinedFunction Outlined;
  SortedVector<efa::BasicBlock> CFG;
  DecodedInstructions Decoded;
};

/// This class, given an Oracle, analyzes a function returning its CFG, the set
/// of callee saved registers, whether it's noreturn or not and its final stack
/// offset
class CFGAnalyzer {
private:
  llvm::Module &M;
  const CSVGlobals &Globals;
  std::unique_ptr<ProgramCounterHandler> PCH;
  FunctionSummaryOracle &Oracle;
  const TupleTree<model::Binary> &Binary;

  llvm::SmallVector<llvm::GlobalVariable *, 16> ABICSVs;

  /// PreCallHook and PostCallHook mark the presence of an original function
  /// call, and surround a basic block containing the registers clobbered by the
  /// function called. They take the MetaAddress of the callee and the
  /// call-site.
  TemporaryOpaqueFunction PreCallHook;
  TemporaryOpaqueFunction PostCallHook;
  TemporaryOpaqueFunction RetHook;

  Outliner Outliner;

  OpaqueFunctionsPool<llvm::Type *> OpaqueBranchConditionsPool;
  std::unique_ptr<llvm::raw_ostream> OutputAAWriter;
  std::unique_ptr<llvm::raw_ostream> OutputIBI;

  /// Where `analyze` records what it reads from the oracle and the model
  PreliminaryAnalysisInputs *Recording = nullptr;

public:
  CFGAnalyzer(llvm::Module &M,
              RootFunction &Root,
              const CSVGlobals &Globals,
              const TupleTree<model::Binary> &Binary,
              FunctionSummaryOracle &Oracle);

public:
  llvm::Function *preCallHook() const { return PreCallHook.get(); }
  llvm::Function *postCallHook() const { return PostCallHook.get(); }
  llvm::Function *retHook() const { return RetHook.get(); }
  const auto &abiCSVs() const { return ABICSVs; }

public:
  /// Analyze the function at \p Entry. If \p Inputs is not null, record in
  /// it every answer the analysis gets from the oracle and the model. The
  /// caller must hold an `UnboundedScanLimits`.
  FunctionSummary analyze(const MetaAddress &Entry,
                          PreliminaryAnalysisInputs *Inputs = nullptr);

  OutlinedFunction outline(const MetaAddress &Entry);

private:
  static llvm::FunctionType *createCallMarkerType(llvm::Module &M);
  static llvm::FunctionType *createRetMarkerType(llvm::Module &M);

private:
  std::optional<UpcastablePointer<efa::FunctionEdgeBase>>
  handleCall(llvm::CallInst *PreCallHookCall);

  SortedVector<efa::BasicBlock> collectDirectCFG(OutlinedFunction *F,
                                                 DecodedInstructions &Decoded);

  void createIBIMarker(OutlinedFunction *F);

  void opaqueBranchConditions(llvm::Function *F, revng::IRBuilder &);

  void materializePCValues(llvm::Function *F, revng::IRBuilder &);

  /// Main thread, before the pipeline: outline the function at \p Entry and
  /// prepare it
  PreparedFunction prepare(const MetaAddress &Entry, llvm::Task &T);

  /// The pipeline: everything it reads is in \p F's module and its arguments
  static void runOptimizationPipeline(llvm::Function &F,
                                      llvm::GlobalVariable *StackPointer,
                                      llvm::raw_ostream &OutputIBI,
                                      llvm::raw_ostream &OutputAAWriter);

  /// Read the calls of \p Marker in \p F, in the order of its use list
  static std::vector<IndirectBranchFacts>
  readIndirectBranchInfo(llvm::Function &F, llvm::Function &Marker);

  /// Main thread, after the pipeline: decide the summary from the facts
  FunctionSummary milkInfo(const MetaAddress &Address,
                           SortedVector<efa::BasicBlock> &&CFG,
                           const DecodedInstructions &Decoded,
                           const std::vector<IndirectBranchFacts> &Branches);

  struct State {
    llvm::Value *StackPointer;
    llvm::Value *ReturnPC;
    llvm::SmallVector<llvm::Value *, 16> CSVs;
  };
  State loadState(revng::IRBuilder &Builder) const;
};

} // namespace efa
