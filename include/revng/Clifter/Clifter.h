#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/Function.h"

#include "revng/Clift/Clift.h"
#include "revng/CliftImportModel/ImportModel.h"
#include "revng/Model/Binary.h"

namespace clift {

class Clifter {
public:
  /// \p Types, if set, converts the model types: share one across the
  /// functions of a module context, so that each type is converted once
  static std::unique_ptr<Clifter> make(mlir::ModuleOp Module,
                                       const model::Binary &Model,
                                       ModelTypeImporter *Types = nullptr);

  virtual ~Clifter() = default;

  virtual clift::FunctionOp import(const llvm::Function *Function) = 0;

protected:
  Clifter() = default;
  Clifter(const Clifter &) = default;
  Clifter &operator=(const Clifter &) = default;
};

} // namespace clift
