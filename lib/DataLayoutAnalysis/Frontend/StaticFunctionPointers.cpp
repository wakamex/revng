//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "StaticFunctionPointers.h"

using namespace llvm;

namespace dla {

StaticFunctionTargets
collectReadOnlyFunctionPointers(const model::Binary &Model,
                                const RawBinaryView &View) {
  StaticFunctionTargets Result;
  // The additional loader protection below follows the Linux x86-64 ABI.
  if (Model.Architecture() != model::Architecture::x86_64
      or Model.OperatingSystem() != model::OperatingSystem::Linux)
    return Result;

  std::map<MetaAddress, std::vector<uint64_t>> RelocationSizes;
  for (const auto &Segment : Model.Segments())
    for (const auto &Relocation : Segment.Relocations())
      RelocationSizes[Relocation.Address()].push_back(Relocation.size());
  for (const auto &Function : Model.ImportedDynamicFunctions())
    for (const auto &Relocation : Function.Relocations())
      RelocationSizes[Relocation.Address()].push_back(Relocation.size());

  for (const auto &Segment : Model.Segments()) {
    for (const auto &Relocation : Segment.Relocations()) {
      MetaAddress Address = Relocation.Address();
      if (Relocation.Type() != model::RelocationType::AddAbsoluteAddress64)
        continue;
      auto End = OverflowSafeInt<uint64_t>(Address.address()) + 8;
      if (not End)
        continue;
      unsigned Overlaps = 0;
      auto Start = Address - std::min(uint64_t(7), Address.address());
      for (auto It = RelocationSizes.lower_bound(Start);
           It != RelocationSizes.end() and It->first < Address + 8;
           ++It)
        for (uint64_t Size : It->second)
          Overlaps += It->first + Size > Address;
      if (Overlaps != 1)
        continue;
      bool Protected = View.isReadOnly(Address, 8)
                       or View.isInRelro(Address, 8);
      if (not Protected or not View.addressToOffset(Address, 8))
        continue;
      MetaAddress Target = Segment.StartAddress() + Relocation.Addend();
      if (not Target.isValid())
        continue;
      Target = MetaAddress::fromPC(Model.Architecture(),
                                   Target.address(),
                                   Target.epoch(),
                                   Target.addressSpace());
      if (Model.Functions().contains(Target))
        Result.emplace(Address, Target);
    }
  }
  return Result;
}

} // namespace dla
