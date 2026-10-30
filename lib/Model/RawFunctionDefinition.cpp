//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "llvm/ADT/STLExtras.h"

#include "revng/Model/ABI.h"
#include "revng/Model/ABI/Definition.h"
#include "revng/Model/RawFunctionDefinition.h"

llvm::SmallVector<const model::NamedTypedRegister *, 8>
model::RawFunctionDefinition::orderedArguments() const {
  std::map<model::Register::Values, unsigned> Rank;
  if (auto Default = model::ABI::getDefaultForELF(Architecture())) {
    const abi::Definition &ABI = abi::Definition::get(*Default);
    for (model::Register::Values Register :
         ABI.GeneralPurposeArgumentRegisters())
      Rank.emplace(Register, Rank.size());
    for (model::Register::Values Register : ABI.VectorArgumentRegisters())
      Rank.emplace(Register, Rank.size());
  }

  llvm::SmallVector<const model::NamedTypedRegister *, 8> Result;
  for (const model::NamedTypedRegister &Argument : Arguments())
    Result.push_back(&Argument);

  // Registers the ABI does not pass arguments in go last, in register order,
  // which is that of `Arguments()`
  auto RankOf = [&Rank](const model::NamedTypedRegister *Argument) {
    auto It = Rank.find(Argument->Location());
    return It == Rank.end() ? Rank.size() : It->second;
  };
  llvm::stable_sort(Result, [&](const auto *LHS, const auto *RHS) {
    return RankOf(LHS) < RankOf(RHS);
  });
  return Result;
}
