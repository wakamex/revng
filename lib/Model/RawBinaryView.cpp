//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Object/ELFObjectFile.h"

#include "revng/Model/RawBinaryView.h"

using namespace llvm;

/// The ranges of an x86-64 Linux ELF's PT_GNU_RELRO headers, in addresses of
/// \p View. The protection follows the Linux x86-64 ABI, so other targets get
/// none.
static std::vector<std::pair<uint64_t, uint64_t>>
readRelro(const model::Binary &Binary, const RawBinaryView &View) {
  std::vector<std::pair<uint64_t, uint64_t>> Result;
  if (Binary.Architecture() != model::Architecture::x86_64 or
      Binary.OperatingSystem() != model::OperatingSystem::Linux)
    return Result;

  auto Bytes = View.data();
  StringRef Buffer(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  auto Object = object::ObjectFile::createObjectFile({Buffer, ""});
  if (not Object) {
    consumeError(Object.takeError());
    return Result;
  }
  auto *ELF = dyn_cast<object::ELF64LEObjectFile>(Object->get());
  if (ELF == nullptr)
    return Result;
  const auto &Header = ELF->getELFFile().getHeader();
  if (Header.e_machine != ELF::EM_X86_64 or
      (Header.e_type != ELF::ET_EXEC and Header.e_type != ELF::ET_DYN))
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
    auto End = OverflowSafeInt<uint64_t>(uint64_t(Header.p_vaddr)) +
               uint64_t(Header.p_memsz);
    // Require a complete final page, without inferring protection for a
    // partially covered page. The start is kept inside the declared region.
    if (not Mapped.isValid() or not End or *End % 4096 != 0)
      continue;
    auto MappedEnd =
        OverflowSafeInt<uint64_t>(Mapped.address()) + uint64_t(Header.p_memsz);
    if (MappedEnd)
      Result.emplace_back(Mapped.address(), *MappedEnd);
  }
  return Result;
}

RawBinaryView::RawBinaryView(const model::Binary &Binary,
                             llvm::ArrayRef<uint8_t> Data)
    : Binary(Binary), Data(Data) {
  Relro = readRelro(Binary, *this);
}

std::optional<llvm::ArrayRef<uint8_t>>
RawBinaryView::getByOffset(uint64_t Offset, uint64_t Size) const {
  auto Sum = OverflowSafeInt(Offset) + Size;
  if (not Sum or *Sum > Data.size())
    return std::nullopt;

  return Data.slice(Offset, Size);
}

std::optional<llvm::ArrayRef<uint8_t>>
RawBinaryView::getByAddress(MetaAddress Address, uint64_t Size) const {
  auto Offset = addressToOffset(Address);
  if (not Offset)
    return std::nullopt;

  return getByOffset(*Offset, Size);
}

std::optional<llvm::StringRef>
RawBinaryView::getStringByAddress(MetaAddress Address, uint64_t Size) const {
  auto BytesOrNone = getByAddress(Address, Size);
  if (not BytesOrNone.has_value())
    return std::nullopt;
  llvm::ArrayRef<uint8_t> Bytes = BytesOrNone.value();
  return llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()), Size);
}

std::optional<uint64_t> RawBinaryView::readInteger(MetaAddress Address,
                                                   uint64_t Size,
                                                   bool IsLittleEndian) const {
  auto MaybeData = getByAddress(Address, Size);
  if (not MaybeData)
    return std::nullopt;

  llvm::support::endianness Endianness;
  if (IsLittleEndian) {
    Endianness = llvm::support::little;
  } else {
    Endianness = llvm::support::big;
  }

  switch (Size) {
  case 1:
    return (*MaybeData)[0];
  case 2:
    return llvm::support::endian::read16(MaybeData->data(), Endianness);
  case 4:
    return llvm::support::endian::read32(MaybeData->data(), Endianness);
  case 8:
    return llvm::support::endian::read64(MaybeData->data(), Endianness);
  default:
    revng_abort("Unexpected read size");
  }
}

std::optional<uint64_t> RawBinaryView::readInteger(MetaAddress Address,
                                                   uint64_t Size) const {
  auto Architecture = Binary.Architecture();
  bool IsLittleEndian = model::Architecture::isLittleEndian(Architecture);
  return readInteger(Address, Size, IsLittleEndian);
}

std::optional<llvm::ArrayRef<uint8_t>>
RawBinaryView::getFromAddressOn(MetaAddress Address) const {
  auto &&[Segment, OffsetInSegment] = findOffsetInSegment(Address, 0);
  if (Segment == nullptr)
    return std::nullopt;

  auto StartOffset = OverflowSafeInt(Segment->StartOffset()) + OffsetInSegment;
  auto Size = OverflowSafeInt(Segment->endOffset()) - StartOffset;
  if (not Size or not StartOffset)
    return std::nullopt;

  return Data.slice(*StartOffset, *Size);
}

/// \note This function ignores the underlying data, it just performs address
///       translation.
std::optional<uint64_t> RawBinaryView::addressToOffset(MetaAddress Address,
                                                       uint64_t Size) const {
  auto &&[Segment, OffsetInSegment] = findOffsetInSegment(Address, Size);
  if (Segment == nullptr) {
    return std::nullopt;
  } else {
    auto Offset = OverflowSafeInt(Segment->StartOffset()) + OffsetInSegment;
    if (not Offset)
      return std::nullopt;

    return *Offset;
  }
}

/// \return the Generic address corresponding to a certain offset, if there's
///         one and only one.
MetaAddress RawBinaryView::offsetToAddress(uint64_t Offset) const {
  using namespace model;

  const Segment *Match = nullptr;
  for (const Segment &Segment : Binary.Segments()) {
    if (Segment.StartOffset() <= Offset and Offset < Segment.endOffset()) {
      if (Match != nullptr) {
        // We have more than one match!
        Match = nullptr;
        break;
      }

      Match = &Segment;
    }
  }

  if (Match != nullptr) {
    auto OffsetInSegment = OverflowSafeInt(Offset) - Match->StartOffset();
    if (OffsetInSegment) {
      return Match->StartAddress() + *OffsetInSegment;
    }
  }

  return MetaAddress::invalid();
}

[[nodiscard]] bool RawBinaryView::isReadOnly(MetaAddress Address,
                                             uint64_t Size) const {
  for (const model::Segment &Segment : Binary.Segments()) {
    if (Segment.contains(Address, Size)) {
      if (!Segment.IsWriteable()) {
        return true;
      }
    }
  }

  return false;
}

[[nodiscard]] bool RawBinaryView::isInRelro(MetaAddress Address,
                                            uint64_t Size) const {
  auto End = OverflowSafeInt(Address.address()) + Size;
  if (not End)
    return false;
  for (auto [Start, Limit] : Relro)
    if (Start <= Address.address() and *End <= Limit)
      return true;
  return false;
}

using SegmentDataPair = std::pair<const model::Segment &,
                                  llvm::ArrayRef<uint8_t>>;
cppcoro::generator<SegmentDataPair> RawBinaryView::segments() const {
  for (const model::Segment &Segment : Binary.Segments()) {
    auto MaybeData = getByOffset(Segment.StartOffset(), Segment.FileSize());
    if (MaybeData)
      co_yield SegmentDataPair(Segment, *MaybeData);
  }
}

MaterializedValue RawBinaryView::load(const MetaAddress &Address,
                                      unsigned LoadSize,
                                      bool IsLittleEndian) const {
  using llvm::APInt;
  auto MaybeValue = readInteger(Address, LoadSize, IsLittleEndian);
  bool IsMutable = not isReadOnly(Address, LoadSize);
  if (MaybeValue) {
    if (IsMutable)
      return MaterializedValue::fromMutable(APInt(LoadSize * 8, *MaybeValue));
    else
      return MaterializedValue::fromConstant(APInt(LoadSize * 8, *MaybeValue));
  } else {
    return MaterializedValue::invalid();
  }
}

std::pair<const model::Segment *, uint64_t>
RawBinaryView::findOffsetInSegment(MetaAddress Address, uint64_t Size) const {
  const model::Segment *Match = nullptr;
  for (const model::Segment &Segment : Binary.Segments()) {
    if (Segment.contains(Address, Size)) {

      if (Match != nullptr) {
        // We have more than one match!
        Match = nullptr;
        break;
      }

      Match = &Segment;
    }
  }

  if (Match != nullptr) {
    auto Offset = OverflowSafeInt(Address.address())
                  - Match->StartAddress().address();
    if (Offset)
      return { Match, *Offset };
  }

  return { nullptr, 0 };
}

llvm::Error RawBinaryView::checkPrecondition(const model::Binary &Binary) {
  llvm::SmallVector<llvm::Error, 0> Errors;
  if (Binary.Binaries().size() != 1)
    Errors.push_back(revng::createError("Binaries should contain exactly 1 "
                                        "entry"));

  for (const model::Segment &Segment : Binary.Segments()) {
    if (not Segment.Binary().isValid())
      Errors.push_back(revng::createError("Segment " + toString(Segment.key())
                                          + " does not reference a valid "
                                            "entry in Binaries"));
  }

  return revng::joinErrors(Errors);
}
