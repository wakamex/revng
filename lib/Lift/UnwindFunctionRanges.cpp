//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TargetSelect.h"

#include "revng/Support/Debug.h"

#include "UnwindFunctionRanges.h"

using namespace llvm;

static Logger Log("unwind-function-ranges");

/// Collect the ranges of the FDEs in the `.eh_frame` section of an ELF file, in
/// the addresses of \p BinaryView
static std::vector<std::tuple<uint64_t, uint64_t, bool>>
readEHFrame(const RawBinaryView &BinaryView,
            ArrayRef<uint8_t> File,
            model::Architecture::Values Architecture) {
  std::vector<std::tuple<uint64_t, uint64_t, bool>> Result;

  StringRef Buffer(reinterpret_cast<const char *>(File.data()), File.size());
  auto MaybeObject = object::ObjectFile::createObjectFile({ Buffer, "" });
  if (not MaybeObject) {
    consumeError(MaybeObject.takeError());
    return Result;
  }

  auto *ELF = dyn_cast<object::ELFObjectFileBase>(MaybeObject->get());
  if (ELF == nullptr)
    return Result;

  for (const object::SectionRef &Section : ELF->sections()) {
    auto MaybeName = Section.getName();
    if (not MaybeName) {
      consumeError(MaybeName.takeError());
      continue;
    }

    if (*MaybeName != ".eh_frame")
      continue;

    auto MaybeContents = Section.getContents();
    if (not MaybeContents) {
      consumeError(MaybeContents.takeError());
      return Result;
    }

    // The unwind information holds link-time addresses: rebase them on where
    // the section lies in the binary as the model maps it
    uint64_t LinkAddress = Section.getAddress();
    uint64_t Offset = object::ELFSectionRef(Section).getOffset();
    MetaAddress Mapped = BinaryView.offsetToAddress(Offset);
    if (Mapped.isInvalid()) {
      revng_log(Log, ".eh_frame is not mapped, ignoring it");
      return Result;
    }
    uint64_t Bias = Mapped.address() - LinkAddress;

    DWARFDataExtractor Data(*MaybeContents,
                            ELF->isLittleEndian(),
                            ELF->getBytesInAddress());
    DWARFDebugFrame Frame(toLLVMArchitecture(Architecture),
                          /* IsEH */ true,
                          LinkAddress);
    if (Error E = Frame.parse(Data)) {
      revng_log(Log, "Cannot parse .eh_frame: " << toString(std::move(E)));
      return {};
    }

    for (const dwarf::FrameEntry &Entry : Frame.entries()) {
      const auto *FDE = dyn_cast<dwarf::FDE>(&Entry);
      if (FDE == nullptr or FDE->getAddressRange() == 0)
        continue;

      // Does the FDE change the unwind state before its first instruction?
      bool StartsMidFrame = false;
      for (const dwarf::CFIProgram::Instruction &I : FDE->cfis()) {
        if (I.Opcode == dwarf::DW_CFA_nop)
          continue;

        StartsMidFrame = not(I.Opcode == dwarf::DW_CFA_advance_loc
                             or I.Opcode == dwarf::DW_CFA_advance_loc1
                             or I.Opcode == dwarf::DW_CFA_advance_loc2
                             or I.Opcode == dwarf::DW_CFA_advance_loc4
                             or I.Opcode == dwarf::DW_CFA_set_loc);
        break;
      }

      uint64_t Start = FDE->getInitialLocation() + Bias;
      Result.emplace_back(Start,
                          Start + FDE->getAddressRange(),
                          StartsMidFrame);
    }

    break;
  }

  return Result;
}

/// Collect the ranges of the `RUNTIME_FUNCTION` entries in the exception
/// directory (`.pdata`) of an x86-64 PE file, in the addresses of
/// \p BinaryView. An entry whose unwind information chains to another entry
/// describes a part of that entry's function, entered with its frame set up;
/// the last element of each tuple is the start of the function's first entry,
/// which the chain ends at.
static std::vector<std::tuple<uint64_t, uint64_t, bool, uint64_t>>
readPData(const RawBinaryView &BinaryView,
          ArrayRef<uint8_t> File,
          uint64_t &ImageBase) {
  std::vector<std::tuple<uint64_t, uint64_t, bool, uint64_t>> Result;

  StringRef Buffer(reinterpret_cast<const char *>(File.data()), File.size());
  auto MaybeObject = object::ObjectFile::createObjectFile({ Buffer, "" });
  if (not MaybeObject) {
    consumeError(MaybeObject.takeError());
    return Result;
  }

  auto *COFF = dyn_cast<object::COFFObjectFile>(MaybeObject->get());
  if (COFF == nullptr or COFF->getMachine() != COFF::IMAGE_FILE_MACHINE_AMD64)
    return Result;

  const object::data_directory *Directory = COFF->getDataDirectory(
    COFF::EXCEPTION_TABLE);
  if (Directory == nullptr or Directory->Size == 0)
    return Result;

  // File offset of the byte at RVA, if a section holds it
  auto ToOffset = [&](uint32_t RVA) -> std::optional<uint64_t> {
    uintptr_t Pointer = 0;
    if (Error E = COFF->getRvaPtr(RVA, Pointer)) {
      consumeError(std::move(E));
      return std::nullopt;
    }
    return Pointer - reinterpret_cast<uintptr_t>(File.data());
  };

  auto DirectoryOffset = ToOffset(Directory->RelativeVirtualAddress);
  if (not DirectoryOffset or *DirectoryOffset + Directory->Size > File.size())
    return Result;

  // The table holds addresses relative to the image base: rebase them on
  // where the table lies in the binary as the model maps it
  MetaAddress Mapped = BinaryView.offsetToAddress(*DirectoryOffset);
  if (Mapped.isInvalid()) {
    revng_log(Log, ".pdata is not mapped, ignoring it");
    return Result;
  }
  uint64_t Bias = Mapped.address() - Directory->RelativeVirtualAddress;
  ImageBase = Bias;

  using namespace llvm::support::endian;
  const uint8_t UNW_FLAG_CHAININFO = 4;
  const size_t EntrySize = 12;
  for (uint64_t I = 0; I + EntrySize <= Directory->Size; I += EntrySize) {
    const uint8_t *Entry = File.data() + *DirectoryOffset + I;
    uint32_t Begin = read32le(Entry);
    uint32_t End = read32le(Entry + 4);
    uint32_t UnwindInfo = read32le(Entry + 8);
    if (Begin == 0 or End <= Begin)
      continue;

    // The low bit set means the entry points to another entry instead
    bool StartsMidFrame = (UnwindInfo & 1) != 0;
    if (not StartsMidFrame) {
      auto UnwindOffset = ToOffset(UnwindInfo);
      if (UnwindOffset and *UnwindOffset < File.size())
        StartsMidFrame = ((File[*UnwindOffset] >> 3) & UNW_FLAG_CHAININFO) != 0;
    }

    // Follow the chain to the function's first entry: an entry pointing to
    // another, or unwind information whose chained entry follows its codes
    uint32_t Primary = Begin;
    uint32_t Link = UnwindInfo;
    for (unsigned Hops = 0; Hops < 32; ++Hops) {
      std::optional<uint64_t> Chained;
      if (Link & 1) {
        Chained = ToOffset(Link & ~1u);
      } else {
        auto UnwindOffset = ToOffset(Link);
        if (not UnwindOffset or *UnwindOffset + 4 > File.size()
            or not((File[*UnwindOffset] >> 3) & UNW_FLAG_CHAININFO))
          break;
        unsigned Codes = File[*UnwindOffset + 2];
        Chained = *UnwindOffset + 4 + 2 * ((Codes + 1) & ~1u);
      }
      if (not Chained or *Chained + EntrySize > File.size())
        break;
      Primary = read32le(File.data() + *Chained);
      Link = read32le(File.data() + *Chained + 8);
    }

    Result.emplace_back(Begin + Bias, End + Bias, StartsMidFrame,
                        Primary + Bias);
  }

  return Result;
}

UnwindFunctionRanges::UnwindFunctionRanges(
  const RawBinaryView &BinaryView,
  ArrayRef<uint8_t> File,
  model::Architecture::Values Architecture) :
  BinaryView(BinaryView),
  Architecture(Architecture) {
  for (auto [Start, End, StartsMidFrame] :
       readEHFrame(BinaryView, File, Architecture))
    Ranges.push_back({ Start, End, StartsMidFrame });

  if (Ranges.empty() and Architecture == model::Architecture::x86_64) {
    for (auto [Start, End, StartsMidFrame, Primary] :
         readPData(BinaryView, File, ImageBase))
      Ranges.push_back({ Start, End, StartsMidFrame, Primary });
    FromPData = not Ranges.empty();
  }

  llvm::sort(Ranges, [](const Range &LHS, const Range &RHS) {
    return std::tie(LHS.Start, LHS.End) < std::tie(RHS.Start, RHS.End);
  });

  // Without a disassembler, the ranges are of no use: we could not tell an
  // instruction start from the middle of an instruction
  if (not Ranges.empty() and not initializeDisassembler(Architecture))
    Ranges.clear();

  revng_log(Log, Ranges.size() << " unwind ranges");
}

UnwindFunctionRanges::~UnwindFunctionRanges() = default;

bool UnwindFunctionRanges::initializeDisassembler(
  model::Architecture::Values Architecture) {
  static std::once_flag Initialized;
  std::call_once(Initialized, [] {
    InitializeAllTargetInfos();
    InitializeAllTargetMCs();
    InitializeAllDisassemblers();
  });

  CodeType = MetaAddressType::defaultCodeFromArch(Architecture);
  auto LLVMArchitecture = toLLVMArchitecture(Architecture);
  std::string TripleName = Triple::getArchTypeName(LLVMArchitecture).str();
  if (LLVMArchitecture == Triple::arm)
    TripleName = "armv7";
  Triple TheTriple(TripleName);

  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(TripleName, Error);
  if (TheTarget == nullptr)
    return false;

  StringRef Features = MetaAddressType::getLLVMCPUFeatures(CodeType);
  RegisterInfo.reset(TheTarget->createMCRegInfo(TripleName));
  MCTargetOptions Options;
  if (RegisterInfo)
    AsmInfo.reset(
      TheTarget->createMCAsmInfo(*RegisterInfo, TripleName, Options));
  SubtargetInfo.reset(
    TheTarget->createMCSubtargetInfo(TripleName, "", Features));
  InstrInfo.reset(TheTarget->createMCInstrInfo());
  if (not RegisterInfo or not AsmInfo or not SubtargetInfo or not InstrInfo)
    return false;

  Context = std::make_unique<MCContext>(TheTriple,
                                        AsmInfo.get(),
                                        RegisterInfo.get(),
                                        SubtargetInfo.get());
  Disassembler.reset(TheTarget->createMCDisassembler(*SubtargetInfo, *Context));
  InstrAnalysis.reset(TheTarget->createMCInstrAnalysis(InstrInfo.get()));
  return Disassembler != nullptr and InstrAnalysis != nullptr;
}

std::optional<unsigned> UnwindFunctionRanges::find(uint64_t Address) const {
  auto It = llvm::upper_bound(Ranges, Address, [](uint64_t A, const Range &R) {
    return A < R.Start;
  });

  if (It == Ranges.begin())
    return std::nullopt;

  --It;
  if (Address >= It->End)
    return std::nullopt;

  return It - Ranges.begin();
}

const UnwindFunctionRanges::Sweep &UnwindFunctionRanges::sweep(unsigned Index) {
  auto It = Sweeps.find(Index);
  if (It != Sweeps.end())
    return It->second;

  const Range &R = Ranges[Index];
  Sweep &Result = Sweeps[Index];
  uint64_t Size = R.End - R.Start;
  Result.InstructionStarts.resize(Size);
  Result.DecodedEnd = R.Start;

  auto Bytes = BinaryView.getByAddress(
    MetaAddress::fromPC(Architecture, R.Start),
    Size);
  if (not Bytes)
    return Result;

  uint64_t Offset = 0;
  while (Offset < Size) {
    MCInst Instruction;
    uint64_t InstructionSize = 0;
    uint64_t Address = R.Start + Offset;
    auto Status = Disassembler->getInstruction(Instruction,
                                               InstructionSize,
                                               Bytes->slice(Offset),
                                               Address,
                                               nulls());
    if (Status != MCDisassembler::Success or InstructionSize == 0)
      break;

    Result.InstructionStarts.set(Offset);

    const MCInstrDesc &Description = InstrInfo->get(Instruction.getOpcode());
    uint64_t Destination = 0;
    if (Description.isBranch() and not Description.isCall()
        and not Description.isIndirectBranch()
        and InstrAnalysis->evaluateBranch(Instruction,
                                          Address,
                                          InstructionSize,
                                          Destination)) {
      Result.DirectJumps.push_back(Destination);
    }

    if (auto Reference = InstrAnalysis
                           ->evaluateMemoryOperandAddress(Instruction,
                                                          SubtargetInfo.get(),
                                                          Address,
                                                          InstructionSize))
      Result.DataReferences.push_back(*Reference);

    Offset += InstructionSize;
  }

  Result.DecodedEnd = R.Start + Offset;
  llvm::sort(Result.DataReferences);
  Result.DataReferences.erase(std::unique(Result.DataReferences.begin(),
                                          Result.DataReferences.end()),
                              Result.DataReferences.end());
  return Result;
}

bool UnwindFunctionRanges::isInstructionInterior(MetaAddress Address) {
  if (Address.type() != CodeType)
    return false;
  if (auto Index = find(Address.address()))
    return not isInstructionStart(*Index, Address.address());
  return false;
}

bool UnwindFunctionRanges::isFunctionEntry(MetaAddress Address) const {
  if (Address.type() != CodeType)
    return false;
  auto Index = find(Address.address());
  return Index and Ranges[*Index].Start == Address.address() and
         not Ranges[*Index].StartsMidFrame;
}

std::vector<MetaAddress> UnwindFunctionRanges::functionEntries() const {
  std::vector<MetaAddress> Result;
  for (const Range &R : Ranges)
    if (not R.StartsMidFrame)
      Result.push_back(MetaAddress::fromPC(Architecture, R.Start));
  return Result;
}

bool UnwindFunctionRanges::isInsideFunction(MetaAddress Address) const {
  if (not FromPData or Address.type() != CodeType)
    return false;
  return find(Address.address()).has_value() and not isFunctionEntry(Address);
}

bool UnwindFunctionRanges::isInstructionStart(unsigned Index,
                                              uint64_t Address) {
  const Range &R = Ranges[Index];
  revng_assert(R.Start <= Address and Address < R.End);
  const Sweep &S = sweep(Index);

  // Past a failed decoding, we cannot tell
  if (Address >= S.DecodedEnd)
    return true;

  return S.InstructionStarts.test(Address - R.Start);
}

/// Whether a direct jump from range \p From to \p Destination, which lies in
/// range \p To, connects two parts of one function rather than being a tail
/// call: the destination part starts mid-frame, or the jump leaves a part
/// starting mid-frame for somewhere other than the start of a function.
static bool joinsParts(bool FromStartsMidFrame,
                       bool ToStartsMidFrame,
                       uint64_t ToStart,
                       uint64_t Destination) {
  return ToStartsMidFrame or (FromStartsMidFrame and Destination != ToStart);
}

void UnwindFunctionRanges::indexParts() {
  if (PartsIndexed)
    return;
  PartsIndexed = true;

  // Record, for each range, the parts that jump into it, so that the group of
  // a range includes parts reached only from a jump table or through a call
  // that never returns
  for (unsigned Index = 0; Index < Ranges.size(); ++Index) {
    if (not Ranges[Index].StartsMidFrame)
      continue;

    for (uint64_t Destination : sweep(Index).DirectJumps) {
      auto To = find(Destination);
      if (not To or *To == Index)
        continue;

      const Range &Target = Ranges[*To];
      if (joinsParts(true, Target.StartsMidFrame, Target.Start, Destination))
        JumpsFromParts[*To].push_back(Index);
    }
  }
}

const std::vector<unsigned> &UnwindFunctionRanges::group(unsigned Index) {
  auto It = Groups.find(Index);
  if (It != Groups.end())
    return It->second;

  // A .pdata part belongs to the function its unwind chain ends at
  if (FromPData) {
    if (PartsByPrimary.empty())
      for (unsigned I = 0; I < Ranges.size(); ++I)
        PartsByPrimary[Ranges[I].Primary].push_back(I);
    std::vector<unsigned> Result = PartsByPrimary.lookup(Ranges[Index].Primary);
    if (llvm::find(Result, Index) == Result.end())
      Result.push_back(Index);
    // The part that begins at a call's state first, as callers expect
    std::stable_partition(Result.begin(), Result.end(), [this](unsigned Part) {
      return not Ranges[Part].StartsMidFrame;
    });
    return Groups[Index] = std::move(Result);
  }

  indexParts();

  std::vector<unsigned> Result = { Index };
  for (size_t I = 0; I < Result.size(); ++I) {
    unsigned Current = Result[I];
    const Range &From = Ranges[Current];

    auto Add = [&Result](unsigned Other) {
      if (llvm::find(Result, Other) == Result.end())
        Result.push_back(Other);
    };

    for (uint64_t Destination : sweep(Current).DirectJumps) {
      auto To = find(Destination);
      if (not To or *To == Current)
        continue;

      const Range &Target = Ranges[*To];
      if (joinsParts(From.StartsMidFrame,
                     Target.StartsMidFrame,
                     Target.Start,
                     Destination))
        Add(*To);
    }

    auto PartsIt = JumpsFromParts.find(Current);
    if (PartsIt != JumpsFromParts.end())
      for (unsigned Part : PartsIt->second)
        Add(Part);
  }

  if (Log.isEnabled() and Result.size() > 1) {
    Log << "Function at 0x" << Twine::utohexstr(Ranges[Index].Start).str()
        << " has " << Result.size() << " parts:";
    for (unsigned Part : Result)
      Log << " 0x" << Twine::utohexstr(Ranges[Part].Start).str();
    Log << DoLog;
  }

  return Groups[Index] = std::move(Result);
}

DecidedBranches UnwindFunctionRanges::makeWalk() {
  return DecidedBranches(*Disassembler,
                         *InstrInfo,
                         *RegisterInfo,
                         [this](uint64_t Address) -> ArrayRef<uint8_t> {
                           auto Bytes = BinaryView.getFromAddressOn(
                             MetaAddress::fromPC(Architecture, Address));
                           return Bytes ? *Bytes : ArrayRef<uint8_t>();
                         },
                         [this](uint64_t Jump)
                           -> std::optional<std::vector<uint64_t>> {
                           auto Table = relativeJumpTable(
                             MetaAddress::fromPC(Architecture, Jump));
                           if (not Table)
                             return std::nullopt;
                           std::vector<uint64_t> Result;
                           for (const MetaAddress &Target : Table->first)
                             Result.push_back(Target.address());
                           return Result;
                         },
                         [this](uint64_t Callee) {
                           return noReturnCallees().contains(Callee);
                         });
}

void UnwindFunctionRanges::recordExits(
  const DecidedBranches::Coverage &Covered) {
  for (uint64_t Exit : Covered.Exits)
    if (not find(Exit))
      GapExits.insert(Exit);
}

const UnwindFunctionRanges::Walked &
UnwindFunctionRanges::walked(unsigned Index) {
  // Walk the function from the part that begins at a call's state; a
  // function whose parts all begin mid-frame is walked from the first part
  const std::vector<unsigned> &Parts = group(Index);
  unsigned Entry = Index;
  for (unsigned Part : Parts) {
    if (not Ranges[Part].StartsMidFrame) {
      Entry = Part;
      break;
    }
  }

  auto It = WalkedByEntry.find(Entry);
  if (It != WalkedByEntry.end())
    return It->second;

  std::vector<std::pair<uint64_t, uint64_t>> Code;
  for (unsigned Part : Parts)
    Code.push_back({ Ranges[Part].Start, Ranges[Part].End });
  DecidedBranches Walk = makeWalk();
  Walked Result;
  DecidedBranches::Coverage Covered;
  Result.Decisions = Walk.walk(Ranges[Entry].Start, Code, &Covered);
  Result.Complete = Covered.Complete;
  Result.Starts.assign(Covered.Starts.begin(), Covered.Starts.end());
  recordExits(Covered);
  if (Log.isEnabled()) {
    Log << "Function at 0x" << Twine::utohexstr(Ranges[Entry].Start).str()
        << " has " << Result.Decisions.size() << " decided jumps:";
    for (auto &[Address, D] : Result.Decisions)
      Log << " 0x" << Twine::utohexstr(Address).str() << (D.Taken ? "+" : "-");
    Log << DoLog;
  }
  return WalkedByEntry.emplace(Entry, std::move(Result)).first->second;
}

std::optional<MetaAddress>
UnwindFunctionRanges::deadSuccessor(MetaAddress Jump) {
  if (Ranges.empty() or Jump.type() != CodeType
      or Architecture != model::Architecture::x86_64)
    return std::nullopt;

  auto Index = find(Jump.address());
  if (not Index)
    return std::nullopt;

  const Walked &W = walked(*Index);
  auto Decision = W.Decisions.find(Jump.address());
  if (Decision == W.Decisions.end())
    return std::nullopt;
  return MetaAddress::fromPC(Architecture, Decision->second.DeadSuccessor);
}

bool UnwindFunctionRanges::isDeadCode(MetaAddress Address) {
  if (Ranges.empty() or Address.type() != CodeType
      or Architecture != model::Architecture::x86_64)
    return false;

  auto Index = find(Address.address());
  if (not Index)
    return false;

  const Walked &W = walked(*Index);
  return W.Complete
         and not std::binary_search(W.Starts.begin(),
                                    W.Starts.end(),
                                    Address.address());
}

std::optional<std::pair<SmallVector<MetaAddress>, SmallVector<uint64_t>>>
UnwindFunctionRanges::relativeJumpTable(MetaAddress Jump) {
  if (Ranges.empty() or Jump.type() != CodeType or
      Architecture != model::Architecture::x86_64)
    return std::nullopt;

  auto Index = find(Jump.address());
  if (not Index or not isInstructionStart(*Index, Jump.address()))
    return std::nullopt;

  const Range &R = Ranges[*Index];
  const Sweep &S = sweep(*Index);
  uint64_t JumpAddress = Jump.address();

  struct Decoded {
    uint64_t Address;
    uint64_t Size;
    MCInst Instruction;
    StringRef Name;
  };
  auto Decode = [&](uint64_t Address) -> std::optional<Decoded> {
    auto Bytes =
        BinaryView.getByAddress(MetaAddress::fromPC(Architecture, Address),
                                std::min<uint64_t>(15, R.End - Address));
    if (not Bytes)
      return std::nullopt;
    Decoded D{Address, 0, MCInst(), StringRef()};
    auto Status = Disassembler->getInstruction(D.Instruction, D.Size, *Bytes,
                                               Address, nulls());
    if (Status != MCDisassembler::Success)
      return std::nullopt;
    D.Name = InstrInfo->getName(D.Instruction.getOpcode());
    return D;
  };

  // The table's address and the form of its entries, matched on the
  // instructions that end at the jump. GCC's entries are signed offsets from
  // the table, which the lea addresses; MSVC's are offsets from the image
  // base, which the lea addresses, and the load adds the table's offset from it
  struct Dispatch {
    uint64_t Table;
    bool FromImageBase;
    int64_t TableOffset;
  };
  auto Match = [&](ArrayRef<Decoded> Instructions) -> std::optional<Dispatch> {
    if (Instructions.empty() or Instructions.back().Name != "JMP64r")
      return std::nullopt;
    unsigned Target = Instructions.back().Instruction.getOperand(0).getReg();

    // Walking back from the jump: X += B, then X = load32(B + I * 4 + D),
    // then B = address of the table or of the image, which MSVC may load once
    // early in the function. Nothing in between may write B, nor X between
    // the load and the jump. MSVC may also load the image base into two
    // registers, one for the load and one for the add: then the add's
    // register is the lea of an address between the load and the add, and the
    // load's base is a lea of the same address.
    auto Writes = [this](const MCInst &I, unsigned Register) {
      const MCInstrDesc &Description = InstrInfo->get(I.getOpcode());
      for (unsigned Def = 0; Def < Description.getNumDefs(); ++Def)
        if (I.getOperand(Def).isReg() and
            RegisterInfo->isSubRegisterEq(I.getOperand(Def).getReg(), Register))
          return true;
      return false;
    };
    auto RIPAddress = [&](const Decoded &D,
                          unsigned Register) -> std::optional<uint64_t> {
      const MCInst &I = D.Instruction;
      if (D.Name != "LEA64r" or I.getOperand(0).getReg() != Register
          or RegisterInfo->getName(I.getOperand(1).getReg()) != StringRef("RIP")
          or I.getOperand(3).getReg() != 0 or not I.getOperand(4).isImm())
        return std::nullopt;
      return D.Address + D.Size + I.getOperand(4).getImm();
    };

    unsigned Base = 0;
    unsigned AddBase = 0;
    std::optional<uint64_t> AddBaseAddress;
    bool FromImageBase = false;
    int64_t TableOffset = 0;
    enum { WantAdd, WantLoad, WantTable } State = WantAdd;
    for (auto It = std::next(Instructions.rbegin()); It != Instructions.rend();
         ++It) {
      const MCInst &I = It->Instruction;
      // Control does not fall through a ret or jmp into the dispatch, which
      // only jumps reach: MSVC's base is the image base they bring
      if (State == WantTable and FromImageBase
          and InstrInfo->get(I.getOpcode()).isBarrier())
        break;
      if (State == WantAdd and
          (It->Name == "ADD64rr" or It->Name == "ADD64rr_REV") and
          I.getOperand(0).getReg() == Target and
          I.getOperand(1).getReg() == Target) {
        Base = AddBase = I.getOperand(2).getReg();
        State = WantLoad;
      } else if (State == WantLoad and not AddBaseAddress
                 and (AddBaseAddress = RIPAddress(*It, AddBase))) {
        Base = 0;
      } else if (State == WantLoad and It->Name == "MOVSX64rm32" and
                 not AddBaseAddress and
                 I.getOperand(0).getReg() == Target and
                 I.getOperand(1).getReg() == Base and
                 I.getOperand(2).getImm() == 4 and
                 I.getOperand(3).getReg() != 0 and I.getOperand(4).isImm() and
                 I.getOperand(4).getImm() == 0 and
                 I.getOperand(5).getReg() == 0) {
        State = WantTable;
      } else if (State == WantLoad and It->Name == "MOV32rm" and
                 RegisterInfo->isSubRegisterEq(Target, I.getOperand(0).getReg())
                 and (AddBaseAddress or I.getOperand(1).getReg() == Base) and
                 I.getOperand(1).getReg() != 0 and
                 I.getOperand(2).getImm() == 4 and
                 I.getOperand(3).getReg() != 0 and I.getOperand(4).isImm() and
                 I.getOperand(5).getReg() == 0) {
        Base = I.getOperand(1).getReg();
        FromImageBase = true;
        TableOffset = I.getOperand(4).getImm();
        State = WantTable;
      } else if (auto Address = State == WantTable ? RIPAddress(*It, Base) :
                                                     std::nullopt) {
        if (AddBaseAddress and *AddBaseAddress != *Address)
          return std::nullopt;
        return Dispatch{ *Address + TableOffset, FromImageBase, TableOffset };
      } else if ((State != WantTable and Writes(I, Target))
                 or (Base != 0 and Writes(I, Base))) {
        return std::nullopt;
      }
    }
    // Adding the register the offset was loaded through is MSVC's dispatch
    // through the image base, which a PE program's relative addresses give:
    // its lea may lie in another part of the function or past bytes that do
    // not decode
    if (State == WantTable and FromImageBase and FromPData
        and (not AddBaseAddress or *AddBaseAddress == ImageBase))
      return Dispatch{ ImageBase + TableOffset, true, TableOffset };
    return std::nullopt;
  };

  // The instructions up to and including the jump, as the sweep of its range
  // decoded them, back far enough to find a base loaded early in the
  // function, or, past a byte the sweep could not decode, as in protected
  // code, from each of the addresses shortly before the jump from which
  // decoding lands on it
  constexpr unsigned Window = 128;
  std::optional<Dispatch> Found;
  if (JumpAddress < S.DecodedEnd) {
    SmallVector<Decoded, Window> Instructions;
    for (uint64_t Offset = JumpAddress - R.Start + 1;
         Offset > 0 and Instructions.size() < Window;) {
      --Offset;
      if (not S.InstructionStarts.test(Offset))
        continue;
      auto D = Decode(R.Start + Offset);
      if (not D)
        return std::nullopt;
      Instructions.push_back(std::move(*D));
    }
    std::reverse(Instructions.begin(), Instructions.end());
    Found = Match(Instructions);
  } else {
    constexpr uint64_t MaxBack = 40;
    for (uint64_t Back = 1; Back <= std::min(MaxBack, JumpAddress - R.Start)
                            and not Found;
         ++Back) {
      SmallVector<Decoded, 16> Instructions;
      uint64_t Address = JumpAddress - Back;
      while (Address < JumpAddress) {
        auto D = Decode(Address);
        if (not D)
          break;
        Address += D->Size;
        Instructions.push_back(std::move(*D));
      }
      if (Address != JumpAddress)
        continue;
      auto D = Decode(JumpAddress);
      if (not D)
        return std::nullopt;
      Instructions.push_back(std::move(*D));
      Found = Match(Instructions);
    }
  }
  if (not Found)
    return std::nullopt;
  std::optional<uint64_t> Table = Found->Table;
  bool FromImageBase = Found->FromImageBase;
  int64_t TableOffset = Found->TableOffset;

  // Read the entries the function's unwind range bounds
  constexpr unsigned MaxEntries = 4096;
  SmallVector<MetaAddress> Targets;
  SmallVector<uint64_t> Entries;
  for (unsigned I = 0; I < MaxEntries; ++I) {
    uint64_t EntryAddress = *Table + 4 * I;
    auto Bytes = BinaryView.getByAddress(
        MetaAddress::fromGeneric(Architecture, EntryAddress), 4);
    if (not Bytes)
      break;
    uint32_t Raw = llvm::support::endian::read32le(Bytes->data());
    uint64_t Destination = FromImageBase ?
                             *Table - TableOffset + Raw :
                             *Table + static_cast<int32_t>(Raw);
    Targets.push_back(MetaAddress::fromPC(Architecture, Destination));
    Entries.push_back(EntryAddress);
  }

  auto Extent = tableExtent(Jump, Targets, Entries);
  if (not Extent or *Extent == 0)
    return std::nullopt;
  Targets.resize(*Extent);
  Entries.resize(*Extent);
  return std::make_pair(std::move(Targets), std::move(Entries));
}

std::optional<size_t>
UnwindFunctionRanges::tableExtent(MetaAddress Jump,
                                  ArrayRef<MetaAddress> Targets,
                                  ArrayRef<uint64_t> Entries) {
  revng_assert(Targets.size() == Entries.size());
  if (Ranges.empty() or Jump.type() != CodeType)
    return std::nullopt;

  auto JumpRange = find(Jump.address());
  if (not JumpRange)
    return std::nullopt;

  const std::vector<unsigned> &Parts = group(*JumpRange);

  // The table ends before the next address after its first entry that the
  // function's code refers to: another table of the function, whose entries,
  // read relative to this table, can point to instructions of the function
  uint64_t End = std::numeric_limits<uint64_t>::max();
  if (not Entries.empty()) {
    for (unsigned Part : Parts) {
      const std::vector<uint64_t> &References = sweep(Part).DataReferences;
      auto It = llvm::upper_bound(References, Entries.front());
      if (It != References.end())
        End = std::min(End, *It);
    }
  }

  size_t Extent = 0;
  for (auto [Target, Entry] : llvm::zip(Targets, Entries)) {
    if (Entry >= End)
      break;

    if (not Target.isValid() or Target.type() != CodeType)
      break;

    auto TargetRange = find(Target.address());
    if (not TargetRange or llvm::find(Parts, *TargetRange) == Parts.end())
      break;

    if (not isInstructionStart(*TargetRange, Target.address()))
      break;

    ++Extent;
  }

  return Extent;
}

std::optional<uint64_t>
UnwindFunctionRanges::directCallBefore(unsigned Index, uint64_t Return) {
  // call rel32
  constexpr uint64_t CallSize = 5;
  constexpr uint8_t CallOpcode = 0xe8;
  const Range &R = Ranges[Index];
  if (Return < R.Start + CallSize or Return > R.End)
    return std::nullopt;

  uint64_t Call = Return - CallSize;
  auto Bytes = BinaryView.getFromAddressOn(MetaAddress::fromPC(Architecture,
                                                               Call));
  if (not Bytes or Bytes->size() < CallSize or (*Bytes)[0] != CallOpcode
      or not isInstructionStart(Index, Call))
    return std::nullopt;

  uint32_t Displacement = 0;
  for (unsigned I = 0; I < 4; ++I)
    Displacement |= uint32_t((*Bytes)[1 + I]) << (8 * I);
  return Return + int64_t(int32_t(Displacement));
}

const llvm::DenseSet<uint64_t> &UnwindFunctionRanges::noReturnCallees() {
  if (NoReturnCallees)
    return *NoReturnCallees;

  NoReturnCallees.emplace();
  if (not FromPData or Architecture != model::Architecture::x86_64)
    return *NoReturnCallees;

  constexpr uint8_t Nop = 0x90;
  for (unsigned Index = 0; Index < Ranges.size(); ++Index) {
    const Range &R = Ranges[Index];
    auto Bytes = BinaryView.getFromAddressOn(MetaAddress::fromPC(Architecture,
                                                                 R.Start));
    if (not Bytes or Bytes->size() < R.End - R.Start)
      continue;

    // The nops that end the range, each an instruction of its own
    uint64_t Return = R.End;
    while (Return > R.Start and (*Bytes)[Return - 1 - R.Start] == Nop)
      --Return;
    if (Return == R.End or not isInstructionStart(Index, Return))
      continue;

    if (auto Callee = directCallBefore(Index, Return))
      NoReturnCallees->insert(*Callee);
  }

  return *NoReturnCallees;
}

bool UnwindFunctionRanges::isNoReturnCallReturn(MetaAddress Address) {
  if (not FromPData or Architecture != model::Architecture::x86_64
      or Address.type() != CodeType or Address.address() == 0)
    return false;

  // The call is in a range; where it returns to can be the range's end
  auto Index = find(Address.address() - 1);
  if (not Index)
    return false;

  auto Callee = directCallBefore(*Index, Address.address());
  return Callee and noReturnCallees().contains(*Callee);
}

bool UnwindFunctionRanges::isGapCode(MetaAddress Address) const {
  return FromPData and Architecture == model::Architecture::x86_64
         and Address.type() == CodeType and not find(Address.address());
}

bool UnwindFunctionRanges::isReachedGapCode(MetaAddress Address,
                                            bool IsRoot) {
  uint64_t A = Address.address();
  if (GapStarts.contains(A))
    return true;
  for (auto [Begin, End] : GapExtents)
    if (Begin <= A and A < End)
      return true;
  if (not IsRoot and not GapExits.contains(A))
    return false;
  if (not GapRoots.insert(A).second)
    return true;

  // The root's code ends where the next function the unwind information
  // describes starts
  auto Next = llvm::upper_bound(Ranges, A, [](uint64_t X, const Range &R) {
    return X < R.Start;
  });
  uint64_t End = Next != Ranges.end() ? Next->Start : A + 0x10000;
  std::pair<uint64_t, uint64_t> Part{ A, End };
  DecidedBranches::Coverage Covered;
  makeWalk().walk(A, Part, &Covered);
  recordExits(Covered);
  if (Covered.Complete)
    GapStarts.insert(Covered.Starts.begin(), Covered.Starts.end());
  else
    GapExtents.push_back(Part);
  revng_log(Log,
            "Gap root 0x" << Twine::utohexstr(A).str() << " reaches "
                          << (Covered.Complete ?
                                std::to_string(Covered.Starts.size())
                                  + " instructions" :
                                "all of its " + std::to_string(End - A)
                                  + " bytes"));
  return true;
}
