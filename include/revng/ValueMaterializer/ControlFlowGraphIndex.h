#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <limits>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

/// A numbered copy of a function's control-flow graph for many region
/// searches while the graph does not change.
///
/// Successors and predecessors are stored as index arrays, so a search reads
/// them directly instead of walking use lists, and visited blocks are marked
/// with a per-search stamp instead of being inserted into a new set.
class ControlFlowGraphIndex {
private:
  std::vector<llvm::BasicBlock *> Blocks;
  llvm::DenseMap<llvm::BasicBlock *, uint32_t> Numbers;
  std::vector<uint32_t> SuccessorOffsets;
  std::vector<uint32_t> SuccessorList;
  std::vector<uint32_t> PredecessorOffsets;
  std::vector<uint32_t> PredecessorList;

  std::vector<uint32_t> ForwardStamps;
  std::vector<uint32_t> BackwardStamps;
  std::vector<uint32_t> BetweenStamps;
  uint32_t Stamp = 0;

public:
  explicit ControlFlowGraphIndex(llvm::Function &F);

  /// Same contract as `insertNodesBetween(Source, Target, Result, Limit)`.
  bool insertBlocksBetween(llvm::BasicBlock *Source,
                           llvm::BasicBlock *Target,
                           llvm::SmallPtrSetImpl<llvm::BasicBlock *> &Result,
                           size_t Limit = std::numeric_limits<size_t>::max());

private:
  uint32_t number(llvm::BasicBlock *BB) const;
  uint32_t nextStamp();
};
