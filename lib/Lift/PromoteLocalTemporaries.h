#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

namespace llvm {
class DominatorTree;
class Function;
} // namespace llvm

/// Promote the entry allocas whose every load follows a store along the chain
/// of single predecessors leading to it, such as the per-block QEMU
/// temporaries of the root, by replacing each load with the value stored last
/// before it. PromoteMemToReg produces the same values for them, but handles
/// each alloca separately; this walks the function once to decide and once to
/// rewrite. Allocas needing a phi, or read before any store, are left as they
/// are.
///
/// \return true if the function changed
bool promoteLocalTemporaries(llvm::Function &F, const llvm::DominatorTree &DT);
