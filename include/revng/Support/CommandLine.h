#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <iosfwd>

#include "llvm/Support/CommandLine.h"

extern llvm::cl::OptionCategory MainCategory;

/// Worker threads for function pipes that can run their expensive part on
/// another thread (1: every function on the calling thread)
extern llvm::cl::opt<unsigned> FunctionPipeJobs;
