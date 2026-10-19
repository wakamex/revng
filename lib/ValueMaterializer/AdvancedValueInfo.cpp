//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <bitset>
#include <cmath>
#include <compare>
#include <iterator>
#include <memory>
#include <optional>
#include <variant>

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/GraphWriter.h"

#include "revng/ADT/ConstantRangeSet.h"
#include "revng/ADT/Queue.h"
#include "revng/MFP/DOTGraphTraits.h"
#include "revng/MFP/Graph.h"
#include "revng/MFP/MFP.h"
#include "revng/Support/GraphAlgorithms.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/Statistics.h"
#include "revng/ValueMaterializer/AdvancedValueInfo.h"
#include "revng/ValueMaterializer/DataFlowGraph.h"
#include "revng/ValueMaterializer/DataFlowRangeAnalysis.h"
#include "revng/ValueMaterializer/Helpers.h"

using namespace llvm;
using std::map;

static Logger AVILogger("avi");

template <>
void mfp::dump(llvm::raw_ostream &Stream, unsigned Indent,
               const AVIConstraints &Element);

inline RunningStatistics AVICFEGSizeStatitistics("avi-cfeg-size");
inline RunningStatistics AVISkippedRegionStatistics("avi-skipped-region-size");

/// Constraint propagation over a region this large spans most of a lifted
/// function: it takes tens of minutes and, in the observed cases, recovers no
/// value. Such requests proceed without oracle constraints.
static constexpr size_t MaxAnalyzedBlocks = 100'000;

ExtensionCandidates::Set ExtensionCandidates::compute(Instruction *I) {
  Set Result;
  Result.insert(I);

  // Steal constraints from trunc'd versions of this value and use the most
  // accurate
  for (User *U : I->users()) {
    if (auto *Trunc = dyn_cast<TruncInst>(U)) {
      Result.insert(Trunc);
      for (User *U2 : Trunc->users()) {
        if (auto *ZExt = dyn_cast<ZExtInst>(U2)) {
          Result.insert(ZExt);
        }
      }
    }
  }

  return Result;
}

const ExtensionCandidates::Set &ExtensionCandidates::get(Instruction *I) {
  auto [It, New] = Cache.try_emplace(I);
  if (New)
    It->second = compute(I);
  return It->second;
}

AdvancedValueInfoMFI::AdvancedValueInfoMFI(LazyValueInfo &LVI,
                                           DataFlowRangeAnalysis &DFRA,
                                           const DominatorTree &DT,
                                           Instruction *Context,
                                           const InstructionsSet &Instructions,
                                           bool ZeroExtendConstraints,
                                           ExtensionCandidates *Extensions) :
  LVI(LVI), DFRA(DFRA), DT(DT), Context(Context), Instructions(Instructions) {
  // The IR is unchanged during this request. Discover candidates once and
  // retain their iteration order across all control-flow nodes and revisits.
  for (Instruction *I : Instructions) {
    if (not ZeroExtendConstraints)
      Candidates[I].insert(I);
    else if (Extensions != nullptr)
      Candidates[I] = Extensions->get(I);
    else
      Candidates[I] = ExtensionCandidates::compute(I);
  }
}

AdvancedValueInfoMFI::LatticeElement
AdvancedValueInfoMFI::combineValues(const LatticeElement &LHS,
                                    const LatticeElement &RHS) const {
  if (not LHS.Reached)
    return RHS;
  if (not RHS.Reached)
    return LHS;

  // An instruction constrained on one side only can take any value on the
  // other, and so in the union
  LatticeElement Result{.Reached = true};
  for (const auto &[LeftEntry, RightEntry] :
       zipmap_range(LHS.Ranges, RHS.Ranges)) {
    if (LeftEntry == nullptr or RightEntry == nullptr)
      continue;
    auto Union = LeftEntry->second.unionWith(RightEntry->second);
    if (not Union.isFullSet())
      Result.Ranges.emplace(LeftEntry->first, std::move(Union));
  }

  return Result;
}

bool AdvancedValueInfoMFI::isLessOrEqual(const LatticeElement &LHS,
                                         const LatticeElement &RHS) const {
  if (not LHS.Reached)
    return true;
  if (not RHS.Reached)
    return false;

  // Every constraint on the right must cover the left, where a missing left
  // entry is the full range
  for (const auto &[LeftEntry, RightEntry] :
       zipmap_range(LHS.Ranges, RHS.Ranges)) {
    if (RightEntry == nullptr)
      continue;
    if (LeftEntry == nullptr or
        not RightEntry->second.contains(LeftEntry->second))
      return false;
  }

  return true;
}

AdvancedValueInfoMFI::LatticeElement
AdvancedValueInfoMFI::applyTransferFunction(Label L,
                                            const LatticeElement &E,
                                            mfp::NoExtraState &) const {

  revng_log(AVILogger, "   " << L->toString());
  LoggerIndent Indent(AVILogger);

  LatticeElement Result = E;
  Result.Reached = true;

  for (Instruction *I : Instructions) {
    uint32_t BitWidth = I->getType()->getIntegerBitWidth();
    ConstantRangeSet Range(BitWidth, true);

    if (AVILogger.isEnabled()) {
      AVILogger << "Considering constraints on " << getName(I);
      if (L->Destination != nullptr) {
        AVILogger << " on " << getName(L->Source) << " -> "
                  << getName(L->Destination);
      } else {
        AVILogger << " in " << getName(L->Source);
      }
      AVILogger << DoLog;
    }

    LoggerIndent Indent(AVILogger);
    for (Instruction *Candidate : Candidates.at(I)) {
      revng_log(AVILogger, "Considering " << getName(Candidate));
      LoggerIndent Indent(AVILogger);
      if (Candidate->getParent() != L->Source
          and not DT.dominates(Candidate, L->Source)) {
        revng_log(AVILogger,
                  "Skipping " << getName(I) << ": not dominated by "
                              << getName(L->Source));
        continue;
      }

      ConstantRangeSet NewRange(Candidate->getType()->getIntegerBitWidth(),
                                true);
      if (L->Destination != nullptr) {
        // TODO: extend DataFlowRangeAnalysis so we no longer need to rely on
        //       LVI too.
        NewRange = LVI.getConstantRangeOnEdge(Candidate,
                                              L->Source,
                                              L->Destination,
                                              Context);

        auto *Branch = dyn_cast<BranchInst>(L->Source->getTerminator());
        if (Branch != nullptr and Branch->isConditional()) {
          BasicBlock *TrueBranch = Branch->getSuccessor(0);
          BasicBlock *FalseBranch = Branch->getSuccessor(1);
          if (TrueBranch != FalseBranch) {
            if (auto MaybeRange = DFRA.visit(*Branch->getCondition(),
                                             *Candidate)) {

              if (TrueBranch == L->Destination) {
                NewRange = NewRange & *MaybeRange;
              } else if (FalseBranch == L->Destination) {
                NewRange = NewRange & ~*MaybeRange;
              }
            }
          }
        }
      } else {
        NewRange = LVI.getConstantRange(Candidate, L->Source->getTerminator());
      }

      if (AVILogger.isEnabled()) {
        AVILogger << "Range: ";
        NewRange.dump(AVILogger);
        AVILogger << DoLog;
      }

      if (NewRange.size().getLimitedValue() < Range.size().getLimitedValue())
        Range = NewRange;
    }
    // Ensure our range is of the right size
    auto Old = Range;
    Range.sextOrTrunc(BitWidth);

    if (AVILogger.isEnabled()) {
      AVILogger << "Range for " << getName(I) << ": ";
      Range.dump(AVILogger);
      AVILogger << DoLog;
    }

    auto It = Result.Ranges.find(I);
    if (It != Result.Ranges.end())
      It->second = It->second.intersectWith(Range);
    else if (not Range.isFullSet())
      Result.Ranges.emplace(I, std::move(Range));
  }

  return Result;
}

void AdvancedValueInfoMFI::dump(GraphType CFEG, const ResultsMap &AllResults) {
  mfp::Graph<AdvancedValueInfoMFI> MFPGraph(CFEG, AllResults);
  llvm::WriteGraph(&MFPGraph, "cfeg");
}

/// \p DFG the data flow graph containing the instructions we're interested
/// in. \p Context the position in the function for the current query.
std::tuple<std::map<llvm::Instruction *, ConstantRangeSet>,
           ControlFlowEdgesGraph, AdvancedValueInfoMFI::ResultsMap>
runAVI(const DataFlowGraph &DFG, llvm::Instruction *Context,
       const llvm::DominatorTree &DT, llvm::LazyValueInfo &LVI,
       DataFlowRangeAnalysis &DFRA, bool ZeroExtendConstraints,
       ControlFlowGraphIndex *CFGIndex, ExtensionCandidates *Extensions) {
  using namespace llvm;

  //
  // Identify nodes from the root of the DFG to all the instructions in the
  // DFG
  //

  SmallPtrSet<BasicBlock *, 4> Whitelist;

  // The containers below keep the order in which the data flow graph yields
  // the instructions: the transfer function queries LVI in this order and the
  // monotone framework starts from the entry points in this order, and LVI's
  // results depend on the order of its queries
  BasicBlock *ContextBB = Context->getParent();
  AdvancedValueInfoMFI::InstructionsSet Targets;

  // Identify all the instructions in the DFG
  for (const DataFlowGraph::Node *Node : post_order(&DFG)) {
    Value *Value = Node->Value;

    // LVI can only work on integer types
    if (not Value->getType()->isIntegerTy())
      continue;

    if (auto *I = dyn_cast<Instruction>(Value)) {
      // Register instruction to be tracked
      Targets.insert(I);
    }
  }

  if (Targets.size() == 0) {
    return {};
  }

  //
  // Collect nodes from which we should start the exploration
  //
  SmallSetVector<BasicBlock *, 4> DFGEntryPoints;

  // Collect in a set all the blocks of the target instructions
  SmallSetVector<BasicBlock *, 4> InstructionBlocks;
  for (Instruction *I : Targets)
    InstructionBlocks.insert(I->getParent());

  // Start from every block that no other target block dominates. Every
  // execution reaching a dominated block has passed through its dominator,
  // while blocks that do not dominate each other can each be reached without
  // the other: the region between one of them and ContextBB covers only the
  // executions passing through it.
  auto IsDominated = [&](BasicBlock *BB) {
    return any_of(InstructionBlocks, [&](BasicBlock *Other) {
      return Other != BB and DT.dominates(Other, BB);
    });
  };

  // Taint all the nodes to go from each entry point to ContextBB, stopping
  // once the region is known to exceed the limit
  SmallPtrSet<BasicBlock *, 16> NodeSet;
  auto Explore = [&](BasicBlock *BB) {
    DFGEntryPoints.insert(BB);
    if (CFGIndex != nullptr)
      return CFGIndex->insertBlocksBetween(BB,
                                           ContextBB,
                                           NodeSet,
                                           MaxAnalyzedBlocks);
    return insertNodesBetween(BB, ContextBB, NodeSet, MaxAnalyzedBlocks);
  };

  bool Fits = true;
  for (BasicBlock *BB : InstructionBlocks)
    if (Fits and not IsDominated(BB))
      Fits = Explore(BB);

  // A dominated block outside those regions is reached only through
  // ContextBB: explore from it as well.
  if (Fits) {
    SmallVector<BasicBlock *, 4> Unreached;
    for (BasicBlock *BB : InstructionBlocks)
      if (IsDominated(BB) and not NodeSet.contains(BB))
        Unreached.push_back(BB);

    for (BasicBlock *BB : Unreached)
      if (Fits)
        Fits = Explore(BB);
  }

  NodeSet.insert(ContextBB);

  if (not Fits or NodeSet.size() > MaxAnalyzedBlocks) {
    // When the search stopped early, NodeSet holds only part of the region
    revng_log(ValueMaterializerLogger,
              "Skipping a CFG subset of at least " << NodeSet.size()
                                                   << " blocks");
    AVISkippedRegionStatistics.push(NodeSet.size());
    return {};
  }

  //
  // Create the subgraph of the CFG containing Whitelist
  //
  auto CFEG = ControlFlowEdgesGraph::fromNodeSet(NodeSet);
  SmallPtrSet<Instruction *, 8> Interesting(Targets.begin(), Targets.end());
  CFEG.setInterestingInstructions(Interesting);

  revng_log(ValueMaterializerLogger,
            "The CFG subset we're interested in has " << CFEG.size()
                                                      << " nodes");
  AVICFEGSizeStatitistics.push(CFEG.size());

  //
  // Run the MFP
  //

  // Identify initial nodes for the monotone framework
  std::vector<const ControlFlowEdgesGraph::Node *> InitialNodes;
  for (BasicBlock *Entry : DFGEntryPoints)
    InitialNodes.push_back(CFEG.at(Entry));

  revng_assert(InitialNodes.size() > 0);

  // Run MFP
  AdvancedValueInfoMFI AVIMFI(LVI,
                              DFRA,
                              DT,
                              Context,
                              Targets,
                              ZeroExtendConstraints,
                              Extensions);

  // Every tracked instruction can take any value on entry
  AdvancedValueInfoMFI::LatticeElement ExtremalValue{.Reached = true};

  mfp::MFPConfiguration<AdvancedValueInfoMFI> Configuration{
    .Instance = &AVIMFI,
    .Flow = &CFEG,
    .ExtremalValue = &ExtremalValue,
    .ExtremalLabels = &InitialNodes,
    .EntryLabels = &InitialNodes,
    .Logger = &AVILogger
  };

  auto GetMaximalFixedPoint = mfp::getMaximalFixedPoint<AdvancedValueInfoMFI>;
  auto AllResults = GetMaximalFixedPoint(Configuration);

  if (AVILogger.isEnabled()) {
    AVILogger << "Dumping MFP results:" << DoLog;
    LoggerIndent Indent(AVILogger);
    for (const auto &[Node, AnalysisResults] : AllResults) {
      AVILogger << Node->toString() << ":\n";
      AVILogger << "  Initial value:\n";
      mfp::dump(*AVILogger.getAsLLVMStream().get(), 2, AnalysisResults.InValue);
      AVILogger << "  Final value:\n";
      mfp::dump(*AVILogger.getAsLLVMStream().get(),
                2,
                AnalysisResults.OutValue);
    }
    AVILogger << DoLog;
  }

  // Report every tracked instruction once the context is reached, the
  // unconstrained ones with the full range
  const auto &OnTarget = AllResults.at(CFEG.at(ContextBB)).OutValue;
  std::map<llvm::Instruction *, ConstantRangeSet> ResultsOnTarget;
  if (OnTarget.Reached) {
    for (Instruction *I : Targets) {
      auto It = OnTarget.Ranges.find(I);
      if (It != OnTarget.Ranges.end())
        ResultsOnTarget.emplace(I, It->second);
      else
        ResultsOnTarget.emplace(
            I, ConstantRangeSet(I->getType()->getIntegerBitWidth(), true));
    }
  }

  return {std::move(ResultsOnTarget), std::move(CFEG), std::move(AllResults)};
}

template <>
void mfp::dump(llvm::raw_ostream &Stream, unsigned Indent,
               const AVIConstraints &Element) {
  if (not Element.Reached) {
    for (unsigned I = 0; I < Indent; ++I)
      Stream << "  ";
    Stream << "unreached\n";
  }
  for (const auto &[I, Range] : Element.Ranges) {
    for (unsigned I = 0; I < Indent; ++I)
      Stream << "  ";
    Stream << getName(I) << ": ";
    Range.dump(Stream, aviFormatter);
    Stream << "\n";
  }
}

template<>
void mfp::dumpLabel(llvm::raw_ostream &Stream,
                    const ControlFlowEdgesGraph::Node *const &Label) {
  Stream << Label->toString();
}
