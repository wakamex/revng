#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class Function;
class GlobalValue;
class LLVMContext;
class Module;
} // namespace llvm

/// The order of the use lists of a function's values, as far as the
/// function's own instructions are the users. Each use is the index of its
/// user among the function's instructions and its operand number. Uses by
/// anything else are not recorded and go last when the order is applied.
struct FunctionUseListOrder {
  using UseList = std::vector<std::pair<unsigned, unsigned>>;

  /// Arguments, blocks and instructions, by their index in that enumeration
  std::vector<std::pair<unsigned, UseList>> Locals;

  /// Globals whose uses by the function could be ordered by walking only as
  /// many uses as the function has, by name
  std::vector<std::pair<std::string, UseList>> Globals;
};

/// A function moved out of its module into a module of its own
struct MovedFunction {
  std::unique_ptr<llvm::Module> Module;
  FunctionUseListOrder Order;
};

/// Move \p F out of its module into a new module in the same context, with
/// the globals it references and \p Extra: global variables keep their
/// initializers, and every global keeps its name, type, attributes and
/// non-debug metadata. Everything an optimization pipeline over \p F reads is
/// then in the new module, except the use-list order, which the bitcode
/// writer only preserves at the cost of walking every shared constant's uses
/// across the original module: it is returned beside the module instead.
MovedFunction moveToOwnModule(llvm::Function &F,
                              llvm::ArrayRef<llvm::GlobalValue *> Extra = {});

/// A function moved to a module of its own and written as bitcode
struct ShippedFunction {
  std::string Name;
  llvm::SmallVector<char, 0> Bitcode;
  FunctionUseListOrder Order;
};

/// Move \p F to a module of its own, write it as bitcode and delete it
ShippedFunction shipFunction(llvm::Function &F,
                             llvm::ArrayRef<llvm::GlobalValue *> Extra = {});

/// Parse \p Shipped into \p Context and restore its use-list order
std::unique_ptr<llvm::Module> receiveFunction(const ShippedFunction &Shipped,
                                              llvm::LLVMContext &Context);
