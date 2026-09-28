#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"

#include "revng/ADT/ConstantRangeSet.h"
#include "revng/MFP/Graph.h"
#include "revng/MFP/MFP.h"
#include "revng/ValueMaterializer/ControlFlowEdgesGraph.h"
#include "revng/ValueMaterializer/ControlFlowGraphIndex.h"
#include "revng/ValueMaterializer/DataFlowRangeAnalysis.h"

namespace llvm {
class Instruction;
class LazyValueInfo;
class DominatorTree;
} // namespace llvm

class DataFlowGraph;

/// The candidates whose constraints refine an instruction's own: the instruction,
/// its truncations and their zero extensions. They depend only on the IR, so one
/// cache serves every request while the IR is unchanged, such as across one
/// pass's value requests.
class ExtensionCandidates {
public:
  using Set = llvm::SmallSet<llvm::Instruction *, 4>;

private:
  llvm::DenseMap<llvm::Instruction *, Set> Cache;

public:
  static Set compute(llvm::Instruction *I);
  const Set &get(llvm::Instruction *I);
};

class AdvancedValueInfoMFI {
public:
  using LatticeElement = std::map<llvm::Instruction *, ConstantRangeSet>;
  using GraphType = const ControlFlowEdgesGraph *;
  using Label = const ControlFlowEdgesGraph::Node *;
  using ResultsMap = std::map<Label, mfp::MFPResult<LatticeElement>>;
  using InstructionsSet = llvm::SmallPtrSetImpl<llvm::Instruction *>;
  using ExtraStateType = mfp::NoExtraState;

private:
  llvm::LazyValueInfo &LVI;
  DataFlowRangeAnalysis &DFRA;
  const llvm::DominatorTree &DT;
  llvm::Instruction *Context;
  llvm::SmallPtrSetImpl<llvm::Instruction *> &Instructions;
  std::map<llvm::Instruction *, ExtensionCandidates::Set> Candidates;

public:
  AdvancedValueInfoMFI(llvm::LazyValueInfo &LVI,
                       DataFlowRangeAnalysis &DFRA,
                       const llvm::DominatorTree &DT,
                       llvm::Instruction *Context,
                       InstructionsSet &Instructions,
                       bool ZeroExtendConstraints,
                       ExtensionCandidates *Extensions = nullptr);

public:
  LatticeElement combineValues(const LatticeElement &LHS,
                               const LatticeElement &RHS) const;

  bool isLessOrEqual(const LatticeElement &LHS,
                     const LatticeElement &RHS) const;

  LatticeElement applyTransferFunction(Label L,
                                       const LatticeElement &E,
                                       mfp::NoExtraState &) const;

public:
  static void dump(GraphType CFEG, const ResultsMap &AllResults);
};

static_assert(mfp::MonotoneFrameworkInstance<AdvancedValueInfoMFI>);

/// \p DFG the data flow graph containing the instructions we're interested in.
/// \p Context the position in the function for the current query.
std::tuple<
  std::map<llvm::Instruction *, ConstantRangeSet>,
  ControlFlowEdgesGraph,
  std::map<const ForwardNode<ControlFlowEdgesNode> *,
           mfp::MFPResult<std::map<llvm::Instruction *, ConstantRangeSet>>>>
runAVI(const DataFlowGraph &DFG,
       llvm::Instruction *Context,
       const llvm::DominatorTree &DT,
       llvm::LazyValueInfo &LVI,
       DataFlowRangeAnalysis &DFRA,
       bool ZeroExtendConstraints,
       ControlFlowGraphIndex *CFGIndex = nullptr,
       ExtensionCandidates *Extensions = nullptr);

template<>
void mfp::dump(llvm::raw_ostream &Stream,
               unsigned Indent,
               const std::map<llvm::Instruction *, ConstantRangeSet> &Element);

template<>
void mfp::dumpLabel(llvm::raw_ostream &Stream,
                    const ControlFlowEdgesGraph::Node *const &Label);
