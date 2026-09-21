#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

namespace llvm {
class Function;
}
class VariableManager;

/// Resolve marked generic-vector helpers while the lifter's CPU-state mapping
/// is available, before register definitions can be discarded by ABI analysis.
void inlineGVecHelpers(llvm::Function &Root, VariableManager &Variables);
