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
                                            "thread. 0, the default, runs "
                                            "every pipe on the calling "
                                            "thread. From 1 up, the per-"
                                            "function pass pipes move each "
                                            "function to a worker through "
                                            "bitcode, and results are the "
                                            "same for every value; "
                                            "collect-cfg's results equal "
                                            "those of 0."),
                                   cl::init(0),
                                   cl::cat(MainCategory));
