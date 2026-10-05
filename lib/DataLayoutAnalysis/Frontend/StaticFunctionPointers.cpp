//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Object/ELFObjectFile.h"

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

  std::vector<std::pair<uint64_t, uint64_t>> Relro;
  auto Bytes = View.data();
  StringRef Buffer(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  auto Object = object::ObjectFile::createObjectFile({ Buffer, "" });
  if (not Object) {
    consumeError(Object.takeError());
    return Result;
  }
  auto *ELF = dyn_cast<object::ELF64LEObjectFile>(Object->get());
  if (ELF == nullptr)
    return Result;
  const auto &Header = ELF->getELFFile().getHeader();
  if (Header.e_machine != ELF::EM_X86_64
      or (Header.e_type != ELF::ET_EXEC and Header.e_type != ELF::ET_DYN))
    return Result;
  auto Headers = ELF->getELFFile().program_headers();
  if (not Headers) {
    consumeError(Headers.takeError());
    return Result;
  }
  for (const auto &Header : *Headers) {
    if (Header.p_type != ELF::PT_GNU_RELRO)
      continue;
    MetaAddress Mapped = View.offsetToAddress(Header.p_offset);
    auto End = OverflowSafeInt<uint64_t>(uint64_t(Header.p_vaddr))
               + uint64_t(Header.p_memsz);
    // Require a complete final page, without inferring protection for a
    // partially covered page. The start is kept inside the declared region.
    if (not Mapped.isValid() or not End or *End % 4096 != 0)
      continue;
    auto MappedEnd = OverflowSafeInt<uint64_t>(Mapped.address())
                     + uint64_t(Header.p_memsz);
    if (MappedEnd)
      Relro.emplace_back(Mapped.address(), *MappedEnd);
  }

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
      bool Protected = View.isReadOnly(Address, 8);
      for (auto [Start, Limit] : Relro)
        Protected |= Start <= Address.address() and *End <= Limit;
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
