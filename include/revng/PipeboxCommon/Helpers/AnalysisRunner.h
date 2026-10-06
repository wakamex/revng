#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <tuple>
#include <utility>

#include "llvm/Support/Error.h"

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/Concepts.h"
#include "revng/PipeboxCommon/Helpers/Helpers.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline::helpers {

/// Helper function that allows running an analysis, deals with unpacking the
/// container list to multiple parameters that will be passed to the run
/// function to of the Analysis.
template<IsAnalysis T, typename... Args>
inline llvm::Error runAnalysis(T &Analysis,
                               Model &TheModel,
                               const Request &Incoming,
                               llvm::StringRef Configuration,
                               std::tuple<Args...> &Containers) {
  using Traits = AnalysisRunTraits<T>;
  revng_assert(Incoming.size() == Traits::ContainerCount);

  // Containers that hold data derived from the model, such as types, update
  // it from this run's model before anything reads them
  std::apply([&](auto &...Container) {
    ([&](auto &C) {
      if constexpr (requires { C.prepareFor(TheModel); })
        C.prepareFor(TheModel);
    }(Container), ...);
  }, Containers);

  using CT = Traits::ContainerTypes;
  return compile_time::callWithIndexSequence<CT>([&]<size_t... I>() {
    return Analysis.run(TheModel,
                        Incoming,
                        Configuration,
                        std::get<I>(Containers)...);
  });
}

} // namespace revng::pypeline::helpers
