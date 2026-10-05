//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <fstream>
#include <iostream>

#include "revng/Support/CommandLine.h"

namespace cl = llvm::cl;

cl::OptionCategory MainCategory("Options", "");

cl::opt<unsigned> FunctionPipeJobs("function-pipe-jobs",
                                   cl::desc("Worker threads for the function "
                                            "pipes that can run their "
                                            "expensive part on another "
                                            "thread. Results and recorded "
                                            "model reads are those of one "
                                            "thread. Enabling a logger forces "
                                            "one."),
                                   cl::init(1),
                                   cl::cat(MainCategory));
