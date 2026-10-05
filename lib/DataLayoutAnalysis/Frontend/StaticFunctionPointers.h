#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "revng/Model/RawBinaryView.h"

namespace dla {

using StaticFunctionTargets = std::map<MetaAddress, MetaAddress>;

/// Function entries stored by unambiguous relocations in read-only memory.
StaticFunctionTargets
collectReadOnlyFunctionPointers(const model::Binary &Model,
                                const RawBinaryView &View);

} // namespace dla
