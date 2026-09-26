//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include "revng/Support/Debug.h"
#include "revng/ValueMaterializer/ControlFlowGraphIndex.h"

using namespace llvm;

ControlFlowGraphIndex::ControlFlowGraphIndex(Function &F) {
  for (BasicBlock &BB : F) {
    Numbers[&BB] = Blocks.size();
    Blocks.push_back(&BB);
  }

  SuccessorOffsets.push_back(0);
  PredecessorOffsets.push_back(0);
  for (BasicBlock *BB : Blocks) {
    for (BasicBlock *Successor : successors(BB))
      SuccessorList.push_back(number(Successor));
    SuccessorOffsets.push_back(SuccessorList.size());

    for (BasicBlock *Predecessor : predecessors(BB))
      PredecessorList.push_back(number(Predecessor));
    PredecessorOffsets.push_back(PredecessorList.size());
  }

  ForwardStamps.resize(Blocks.size());
  BackwardStamps.resize(Blocks.size());
  BetweenStamps.resize(Blocks.size());
}

uint32_t ControlFlowGraphIndex::number(BasicBlock *BB) const {
  auto It = Numbers.find(BB);
  revng_assert(It != Numbers.end());
  return It->second;
}

uint32_t ControlFlowGraphIndex::nextStamp() {
  if (Stamp == std::numeric_limits<uint32_t>::max()) {
    std::fill(ForwardStamps.begin(), ForwardStamps.end(), 0);
    std::fill(BackwardStamps.begin(), BackwardStamps.end(), 0);
    std::fill(BetweenStamps.begin(), BetweenStamps.end(), 0);
    Stamp = 0;
  }
  return ++Stamp;
}

bool ControlFlowGraphIndex::insertBlocksBetween(BasicBlock *SourceBlock,
                                                BasicBlock *TargetBlock,
                                                SmallPtrSetImpl<BasicBlock *>
                                                  &Result,
                                                size_t Limit) {
  if (SourceBlock == TargetBlock) {
    Result.insert(SourceBlock);
    return Limit >= 1;
  }

  uint32_t Source = number(SourceBlock);
  uint32_t Target = number(TargetBlock);
  uint32_t Current = nextStamp();

  struct Search {
    const std::vector<uint32_t> &Offsets;
    const std::vector<uint32_t> &List;
    std::vector<uint32_t> &Stamps;
    SmallVector<uint32_t, 16> Stack;
  };

  // Visit the children of one block, never entering Stop and, when
  // WithinStamps is not null, only entering blocks it marks with Current.
  // Calls Found on each newly visited block.
  auto Step = [Current](Search &S,
                        uint32_t Stop,
                        const std::vector<uint32_t> *WithinStamps,
                        auto &&Found) {
    uint32_t Node = S.Stack.pop_back_val();
    for (uint32_t I = S.Offsets[Node]; I < S.Offsets[Node + 1]; ++I) {
      uint32_t Child = S.List[I];
      if (Child == Stop or S.Stamps[Child] == Current)
        continue;
      if (WithinStamps != nullptr and (*WithinStamps)[Child] != Current)
        continue;
      S.Stamps[Child] = Current;
      S.Stack.push_back(Child);
      Found(Child);
    }
  };

  Search Forward{ SuccessorOffsets, SuccessorList, ForwardStamps, { Source } };
  Search Backward{
    PredecessorOffsets, PredecessorList, BackwardStamps, { Target }
  };
  ForwardStamps[Source] = Current;
  BackwardStamps[Target] = Current;

  // Blocks found by both searches lie between Source and Target, which then
  // also belong to the result
  size_t Common = 0;
  while (not Forward.Stack.empty() and not Backward.Stack.empty()) {
    Step(Forward, Target, nullptr, [&](uint32_t Block) {
      Common += BackwardStamps[Block] == Current;
    });
    Step(Backward, Source, nullptr, [&](uint32_t Block) {
      Common += ForwardStamps[Block] == Current;
    });
    if (Common > 0 and Common + 2 > Limit)
      return false;
  }

  // Search again, only within the blocks the finished search found
  SmallVector<uint32_t, 16> Between;
  auto Collect = [&Between](uint32_t Block) { Between.push_back(Block); };
  if (Backward.Stack.empty()) {
    Search Within{ SuccessorOffsets, SuccessorList, BetweenStamps, { Source } };
    BetweenStamps[Source] = Current;
    while (not Within.Stack.empty())
      Step(Within, Target, &BackwardStamps, Collect);
  } else {
    Search Within{
      PredecessorOffsets, PredecessorList, BetweenStamps, { Target }
    };
    BetweenStamps[Target] = Current;
    while (not Within.Stack.empty())
      Step(Within, Source, &ForwardStamps, Collect);
  }

  bool DirectEdge = false;
  for (uint32_t I = SuccessorOffsets[Source]; I < SuccessorOffsets[Source + 1];
       ++I)
    DirectEdge = DirectEdge or SuccessorList[I] == Target;

  if (not Between.empty() or DirectEdge) {
    if (Between.size() + 2 > Limit)
      return false;
    for (uint32_t Block : Between)
      Result.insert(Blocks[Block]);
    Result.insert(SourceBlock);
    Result.insert(TargetBlock);
  }

  return true;
}
