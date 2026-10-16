//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <limits>
#include <mutex>

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

UnwindFunctionRanges::UnwindFunctionRanges(
  const RawBinaryView &BinaryView,
  ArrayRef<uint8_t> File,
  model::Architecture::Values Architecture) :
  BinaryView(BinaryView),
  Architecture(Architecture) {
  for (auto [Start, End, StartsMidFrame] :
       readEHFrame(BinaryView, File, Architecture))
    Ranges.push_back({ Start, End, StartsMidFrame });

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

std::optional<std::pair<SmallVector<MetaAddress>, SmallVector<uint64_t>>>
UnwindFunctionRanges::relativeJumpTable(MetaAddress Jump) {
  if (Ranges.empty() or Jump.type() != CodeType or
      Architecture != model::Architecture::x86_64)
    return std::nullopt;

  auto Index = find(Jump.address());
  if (not Index or not isInstructionStart(*Index, Jump.address()))
    return std::nullopt;

  // Decode up to the last few instructions up to and including the jump
  const Range &R = Ranges[*Index];
  const Sweep &S = sweep(*Index);
  if (Jump.address() >= S.DecodedEnd)
    return std::nullopt;

  constexpr unsigned Window = 8;
  SmallVector<uint64_t, Window> Starts;
  for (uint64_t Offset = Jump.address() - R.Start + 1;
       Offset > 0 and Starts.size() < Window;) {
    --Offset;
    if (S.InstructionStarts.test(Offset))
      Starts.push_back(R.Start + Offset);
  }
  std::reverse(Starts.begin(), Starts.end());

  struct Decoded {
    uint64_t Address;
    uint64_t Size;
    MCInst Instruction;
    StringRef Name;
  };
  SmallVector<Decoded, Window> Instructions;
  for (uint64_t Address : Starts) {
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
    Instructions.push_back(std::move(D));
  }

  if (Instructions.empty() or Instructions.back().Name != "JMP64r")
    return std::nullopt;
  unsigned Target = Instructions.back().Instruction.getOperand(0).getReg();

  // Walking back from the jump: X += B, then X = sext(load32(B + I * 4)), then
  // B = address of the table. Nothing else in between may write X or B.
  auto Writes = [this](const MCInst &I, unsigned Register) {
    const MCInstrDesc &Description = InstrInfo->get(I.getOpcode());
    for (unsigned Def = 0; Def < Description.getNumDefs(); ++Def)
      if (I.getOperand(Def).isReg() and
          RegisterInfo->isSubRegisterEq(I.getOperand(Def).getReg(), Register))
        return true;
    return false;
  };

  unsigned Base = 0;
  std::optional<uint64_t> Table;
  enum { WantAdd, WantLoad, WantTable } State = WantAdd;
  for (auto It = std::next(Instructions.rbegin()); It != Instructions.rend();
       ++It) {
    const MCInst &I = It->Instruction;
    if (State == WantAdd and It->Name == "ADD64rr" and
        I.getOperand(0).getReg() == Target and
        I.getOperand(1).getReg() == Target) {
      Base = I.getOperand(2).getReg();
      State = WantLoad;
    } else if (State == WantLoad and It->Name == "MOVSX64rm32" and
               I.getOperand(0).getReg() == Target and
               I.getOperand(1).getReg() == Base and
               I.getOperand(2).getImm() == 4 and
               I.getOperand(3).getReg() != 0 and I.getOperand(4).isImm() and
               I.getOperand(4).getImm() == 0 and
               I.getOperand(5).getReg() == 0) {
      State = WantTable;
    } else if (State == WantTable and It->Name == "LEA64r" and
               I.getOperand(0).getReg() == Base and
               RegisterInfo->getName(I.getOperand(1).getReg()) ==
                   StringRef("RIP") and
               I.getOperand(3).getReg() == 0 and I.getOperand(4).isImm()) {
      Table = It->Address + It->Size + I.getOperand(4).getImm();
      break;
    } else if (Writes(I, Target) or (Base != 0 and Writes(I, Base))) {
      return std::nullopt;
    }
  }
  if (not Table)
    return std::nullopt;

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
    int32_t Offset =
        static_cast<int32_t>(llvm::support::endian::read32le(Bytes->data()));
    Targets.push_back(MetaAddress::fromPC(Architecture, *Table + Offset));
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
