#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/PipeboxCommon/BinariesContainer.h"
#include "revng/PipeboxCommon/LLVMContainer.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline::analyses {

class AnalyzeDataLayout {
public:
  static constexpr llvm::StringRef Name = "analyze-data-layout";

  llvm::Error run(Model &Model,
                  const Request &Incoming,
                  llvm::StringRef Configuration,
                  const BinariesContainer &Binaries,
                  LLVMFunctionContainer &ModuleContainer);
};

} // namespace revng::pypeline::analyses
