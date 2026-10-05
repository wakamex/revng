#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class ScalarEvolution;
class SCEV;
} // end namespace llvm

namespace dla {
struct LayoutTypeSystemNode;
} // end namespace dla

/// Class useful to explore an llvm::SCEV expression to find its base
/// addresses.
class SCEVBaseAddressExplorer {
public:
  using SCEVTypeMap = std::map<const llvm::SCEV *, dla::LayoutTypeSystemNode *>;

private:
  llvm::SmallVector<const llvm::SCEV *, 4> Worklist;

public:
  SCEVBaseAddressExplorer() = default;
  ~SCEVBaseAddressExplorer() = default;

  /// Returns the SCEVs of \Root 's base addresses, each once, in the order
  /// the exploration finds them.
  //
  // The function works exploring the AST of the SCEV, going from the \Root
  // towards its operands.
  // If \M is not empty, all the SCEVs with an entry in \M are considered as
  // addresses, and the exploration of the operands does not traverse them, even
  // if the SCEV potentially has the expressive power to do it.
  //
  // The order is the order of the operands of the SCEV expressions, which
  // does not depend on where the SCEVs are allocated: the caller creates a
  // layout node for each base address it has not seen yet, so an order of
  // addresses would number the nodes differently from run to run.
  llvm::SmallSetVector<const llvm::SCEV *, 4>
  findBases(llvm::ScalarEvolution *SE,
            const llvm::SCEV *Root,
            const SCEVTypeMap &M);

private:
  size_t checkAddressOrTraverse(llvm::ScalarEvolution *SE, const llvm::SCEV *S);
};
