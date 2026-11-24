//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Lift/DecidedBranches.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

using namespace llvm;

static Logger Log("decided-branches");
/// Every step, decision and arm fact of the walk with its states, for a
/// symbolic check of each one (verify_walk.py --cpp-log)
static Logger StatesLog("decided-branches-states");

namespace {

constexpr uint64_t Full = ~uint64_t(0);

enum Flag : unsigned { CF, ZF, SF, PF, OF, FlagCount };
enum Fact : unsigned { BE, L, LE, FactCount };

/// A value or flag that may be unknown
using Tri = std::optional<bool>;

Tri triNot(Tri A) {
  if (not A)
    return std::nullopt;
  return not *A;
}

Tri triOr(Tri A, Tri B) {
  if ((A and *A) or (B and *B))
    return true;
  if (A and B)
    return false;
  return std::nullopt;
}

Tri triNe(Tri A, Tri B) {
  if (not A or not B)
    return std::nullopt;
  return *A != *B;
}

uint64_t widthMask(unsigned Bits) {
  return Bits >= 64 ? Full : (uint64_t(1) << Bits) - 1;
}

bool evenParity(uint64_t Value) {
  return __builtin_popcountll(Value & 0xFF) % 2 == 0;
}

/// The family and width an LLVM x86 opcode name starts with, and the rest:
/// `ADD32ri8` is ("ADD", 32, "ri8")
struct OpcodeName {
  StringRef Family;
  unsigned Bits = 0;
  StringRef Form;
};

OpcodeName parseName(StringRef Name) {
  OpcodeName Result;
  size_t DigitStart = Name.find_first_of("0123456789");
  if (DigitStart == StringRef::npos) {
    Result.Family = Name;
    return Result;
  }
  Result.Family = Name.take_front(DigitStart);
  StringRef Rest = Name.drop_front(DigitStart);
  size_t DigitEnd = Rest.find_first_not_of("0123456789");
  StringRef Digits = Rest.take_front(DigitEnd);
  Digits.getAsInteger(10, Result.Bits);
  Result.Form = DigitEnd == StringRef::npos ? StringRef() :
                                              Rest.drop_front(DigitEnd);
  return Result;
}

} // namespace

/// A path's knowledge
struct DecidedBranches::State {
  std::array<Tri, FlagCount> Flags;
  std::array<Tri, FactCount> Facts;
  /// 64-bit register -> (value, mask of known bits)
  std::map<unsigned, std::pair<uint64_t, uint64_t>> Registers;
  /// rsp minus its value at entry, when known
  std::optional<int64_t> StackOffset = 0;
  /// Known bytes at offsets from rsp's value at entry
  std::map<int64_t, uint8_t> Stack;

  bool operator==(const State &Other) const = default;

  /// What holds on both
  State join(const State &Other) const {
    State Result;
    for (unsigned I = 0; I < FlagCount; ++I)
      Result.Flags[I] = Flags[I] == Other.Flags[I] ? Flags[I] : std::nullopt;
    for (unsigned I = 0; I < FactCount; ++I)
      Result.Facts[I] = Facts[I] == Other.Facts[I] ? Facts[I] : std::nullopt;
    for (auto &[Register, Known] : Registers) {
      auto It = Other.Registers.find(Register);
      if (It == Other.Registers.end())
        continue;
      auto [V1, M1] = Known;
      auto [V2, M2] = It->second;
      uint64_t Mask = M1 & M2 & ~(V1 ^ V2);
      if (Mask != 0)
        Result.Registers[Register] = { V1 & Mask, Mask };
    }
    Result.StackOffset = StackOffset == Other.StackOffset ? StackOffset :
                                                            std::nullopt;
    if (Result.StackOffset)
      for (auto &[Offset, Byte] : Stack) {
        auto It = Other.Stack.find(Offset);
        if (It != Other.Stack.end() and It->second == Byte)
          Result.Stack[Offset] = Byte;
      }
    return Result;
  }
};

namespace {

/// The outcome of each condition code from the flags and facts, and what each
/// outcome implies
struct Condition {
  Tri (*Outcome)(const DecidedBranches::State &);
  std::vector<std::pair<unsigned, bool>> TakenFlags;
  std::vector<std::pair<unsigned, bool>> TakenFacts;
  std::vector<std::pair<unsigned, bool>> NotTakenFlags;
  std::vector<std::pair<unsigned, bool>> NotTakenFacts;
};

Tri belowOrEqual(const DecidedBranches::State &S) {
  return S.Facts[BE] ? S.Facts[BE] : triOr(S.Flags[CF], S.Flags[ZF]);
}

Tri less(const DecidedBranches::State &S) {
  return S.Facts[L] ? S.Facts[L] : triNe(S.Flags[SF], S.Flags[OF]);
}

Tri lessOrEqual(const DecidedBranches::State &S) {
  return S.Facts[LE] ? S.Facts[LE] : triOr(S.Flags[ZF], less(S));
}

using St = DecidedBranches::State;

/// Indexed by X86::CondCode: O NO B AE E NE BE A S NS P NP L GE LE G
const Condition Conditions[16] = {
  { [](const St &S) { return S.Flags[OF]; }, { { OF, true } }, {}, { { OF, false } }, {} },
  { [](const St &S) { return triNot(S.Flags[OF]); }, { { OF, false } }, {}, { { OF, true } }, {} },
  { [](const St &S) { return S.Flags[CF]; }, { { CF, true } }, { { BE, true } }, { { CF, false } }, {} },
  { [](const St &S) { return triNot(S.Flags[CF]); }, { { CF, false } }, {}, { { CF, true } }, { { BE, true } } },
  { [](const St &S) { return S.Flags[ZF]; }, { { ZF, true } }, { { BE, true }, { LE, true } }, { { ZF, false } }, {} },
  { [](const St &S) { return triNot(S.Flags[ZF]); }, { { ZF, false } }, {}, { { ZF, true } }, { { BE, true }, { LE, true } } },
  { belowOrEqual, {}, { { BE, true } }, { { CF, false }, { ZF, false } }, { { BE, false } } },
  { [](const St &S) { return triNot(belowOrEqual(S)); }, { { CF, false }, { ZF, false } }, { { BE, false } }, {}, { { BE, true } } },
  { [](const St &S) { return S.Flags[SF]; }, { { SF, true } }, {}, { { SF, false } }, {} },
  { [](const St &S) { return triNot(S.Flags[SF]); }, { { SF, false } }, {}, { { SF, true } }, {} },
  { [](const St &S) { return S.Flags[PF]; }, { { PF, true } }, {}, { { PF, false } }, {} },
  { [](const St &S) { return triNot(S.Flags[PF]); }, { { PF, false } }, {}, { { PF, true } }, {} },
  { less, {}, { { L, true }, { LE, true } }, {}, { { L, false } } },
  { [](const St &S) { return triNot(less(S)); }, {}, { { L, false } }, {}, { { L, true }, { LE, true } } },
  { lessOrEqual, {}, { { LE, true } }, { { ZF, false } }, { { LE, false }, { L, false } } },
  { [](const St &S) { return triNot(lessOrEqual(S)); }, { { ZF, false } }, { { LE, false }, { L, false } }, {}, { { LE, true } } },
};

St implied(const St &S, unsigned Code, bool Taken) {
  St Result = S;
  const Condition &C = Conditions[Code];
  for (auto [Flag, Value] : Taken ? C.TakenFlags : C.NotTakenFlags)
    Result.Flags[Flag] = Value;
  for (auto [Fact, Value] : Taken ? C.TakenFacts : C.NotTakenFacts)
    Result.Facts[Fact] = Value;
  return Result;
}

/// A logical operand: a register, an immediate, or a memory reference
struct Operand {
  enum Kind { Register, Immediate, Memory } K = Register;
  unsigned Reg = 0;
  int64_t Imm = 0;
  unsigned Base = 0;
  unsigned Index = 0;
  int64_t Scale = 1;
  int64_t Displacement = 0;
};

} // namespace

DecidedBranches::DecidedBranches(const MCDisassembler &Disassembler,
                                 const MCInstrInfo &InstrInfo,
                                 const MCRegisterInfo &RegisterInfo,
                                 BytesAt Bytes,
                                 IndirectTargets Indirect,
                                 NoReturnCallee NoReturn) :
  Disassembler(Disassembler),
  InstrInfo(InstrInfo),
  RegisterInfo(RegisterInfo),
  Bytes(std::move(Bytes)),
  Indirect(std::move(Indirect)),
  NoReturn(std::move(NoReturn)) {

  // Name the 64-bit registers and the parts of them the walk tracks
  StringMap<unsigned> ByName;
  for (unsigned Reg = 1; Reg < RegisterInfo.getNumRegs(); ++Reg)
    ByName[RegisterInfo.getName(Reg)] = Reg;

  auto Add = [&](StringRef Name, StringRef Base, unsigned Shift,
                 unsigned Bits) {
    auto It = ByName.find(Name);
    auto BaseIt = ByName.find(Base);
    if (It != ByName.end() and BaseIt != ByName.end())
      Slices[It->second] = { BaseIt->second, Shift, Bits };
  };
  for (std::string Letter : { "A", "B", "C", "D" }) {
    std::string R = "R" + Letter + "X";
    Add(R, R, 0, 64);
    Add("E" + Letter + "X", R, 0, 32);
    Add(Letter + "X", R, 0, 16);
    Add(Letter + "L", R, 0, 8);
    Add(Letter + "H", R, 8, 8);
  }
  for (std::string Name : { "SI", "DI", "BP", "SP" }) {
    std::string R = "R" + Name;
    Add(R, R, 0, 64);
    Add("E" + Name, R, 0, 32);
    Add(Name, R, 0, 16);
    Add(Name + "L", R, 0, 8);
  }
  for (unsigned N = 8; N < 16; ++N) {
    std::string R = "R" + std::to_string(N);
    Add(R, R, 0, 64);
    Add(R + "D", R, 0, 32);
    Add(R + "W", R, 0, 16);
    Add(R + "B", R, 0, 8);
  }
  RSP = ByName.lookup("RSP");
  revng_assert(RSP != 0);
  CL = ByName.lookup("CL");
  revng_assert(CL != 0);
}

/// Applies instructions to a state
class DecidedBranches::Interpreter {
private:
  const DecidedBranches &D;
  const MCInst &I;
  const MCInstrDesc &Desc;
  OpcodeName Name;
  uint64_t Address;
  uint64_t Size;
  State &S;
  std::vector<Operand> Operands;
  /// The flags the instruction set, one bit each
  unsigned Touched = 0;

public:
  Interpreter(const DecidedBranches &D,
              const MCInst &I,
              uint64_t Address,
              uint64_t Size,
              State &S) :
    D(D),
    I(I),
    Desc(D.InstrInfo.get(I.getOpcode())),
    Name(parseName(D.InstrInfo.getName(I.getOpcode()))),
    Address(Address),
    Size(Size),
    S(S) {
    collectOperands();
  }

  /// Apply the instruction; false ends the path
  bool run();

  /// Whether the instruction set \p Flag, to a known value or not
  bool touched(unsigned Flag) const { return Touched & (1u << Flag); }

private:
  /// The logical operands: tied copies are left out and a memory reference's
  /// five MC operands are one. The address of a lea into a 32-bit register
  /// (LEA64_32r) is a memory reference LLVM does not mark as one.
  void collectOperands() {
    for (unsigned Index = 0; Index < I.getNumOperands(); ++Index) {
      if (Index < Desc.getNumOperands()) {
        if (Desc.getOperandConstraint(Index, MCOI::TIED_TO) != -1)
          continue;
        bool Memory = Desc.operands()[Index].OperandType
                        == MCOI::OPERAND_MEMORY
                      or (Name.Family == "LEA" and Index == 1);
        if (Memory and Index + 4 < I.getNumOperands()) {
          Operand M;
          M.K = Operand::Memory;
          M.Base = I.getOperand(Index).getReg();
          M.Scale = I.getOperand(Index + 1).getImm();
          M.Index = I.getOperand(Index + 2).getReg();
          M.Displacement = I.getOperand(Index + 3).isImm() ?
                             I.getOperand(Index + 3).getImm() :
                             0;
          Operands.push_back(M);
          Index += 4;
          continue;
        }
      }
      const MCOperand &Op = I.getOperand(Index);
      Operand O;
      if (Op.isReg()) {
        O.K = Operand::Register;
        O.Reg = Op.getReg();
      } else if (Op.isImm()) {
        O.K = Operand::Immediate;
        O.Imm = Op.getImm();
      } else {
        continue;
      }
      Operands.push_back(O);
    }
  }

  // Registers

  std::pair<uint64_t, uint64_t> readRegister(unsigned Reg) const {
    auto It = D.Slices.find(Reg);
    if (It == D.Slices.end())
      return { 0, 0 };
    auto [Base, Shift, Bits] = It->second;
    auto Known = S.Registers.find(Base);
    if (Known == S.Registers.end())
      return { 0, 0 };
    uint64_t Mask = widthMask(Bits);
    return { (Known->second.first >> Shift) & Mask,
             (Known->second.second >> Shift) & Mask };
  }

  void writeRegister(unsigned Reg, uint64_t Value, uint64_t Mask) {
    auto It = D.Slices.find(Reg);
    if (It == D.Slices.end())
      return;
    auto [Base, Shift, Bits] = It->second;
    uint64_t Width = widthMask(Bits);
    Value &= Width;
    Mask &= Width;
    uint64_t OldValue = 0;
    uint64_t OldMask = 0;
    if (Bits == 32) {
      // A 32-bit write zeroes the upper half
      OldMask = Full & ~Width;
    } else if (auto Known = S.Registers.find(Base);
               Known != S.Registers.end()) {
      OldMask = Known->second.second & ~(Width << Shift);
      OldValue = Known->second.first & OldMask;
    }
    uint64_t NewMask = OldMask | (Mask << Shift);
    uint64_t NewValue = (OldValue | (Value << Shift)) & NewMask;
    if (NewMask != 0)
      S.Registers[Base] = { NewValue, NewMask };
    else
      S.Registers.erase(Base);
    if (Base == D.RSP)
      S.StackOffset.reset();
  }

  void forgetRegister(unsigned Reg) {
    auto It = D.Slices.find(Reg);
    if (It == D.Slices.end())
      return;
    writeRegister(Reg, 0, 0);
  }

  // Memory

  /// The offset from rsp's value at entry a memory operand addresses, when
  /// known
  std::optional<int64_t> stackAddress(const Operand &M) const {
    if (M.Base == D.RSP and M.Index == 0 and S.StackOffset)
      return *S.StackOffset + M.Displacement;
    return std::nullopt;
  }

  bool isRipRelative(const Operand &M) const {
    return M.Base != 0 and D.RegisterInfo.getName(M.Base) == StringRef("RIP");
  }

  std::pair<uint64_t, uint64_t> readMemory(const Operand &M,
                                           unsigned Bits) const {
    auto Offset = stackAddress(M);
    if (not Offset)
      return { 0, 0 };
    uint64_t Value = 0;
    uint64_t Mask = 0;
    for (unsigned Byte = 0; Byte < Bits / 8; ++Byte) {
      auto It = S.Stack.find(*Offset + Byte);
      if (It != S.Stack.end()) {
        Value |= uint64_t(It->second) << (8 * Byte);
        Mask |= uint64_t(0xFF) << (8 * Byte);
      }
    }
    return { Value, Mask };
  }

  void writeMemory(const Operand &M,
                   unsigned Bits,
                   uint64_t Value,
                   uint64_t Mask) {
    auto Offset = stackAddress(M);
    if (Offset) {
      for (unsigned Byte = 0; Byte < Bits / 8; ++Byte) {
        if (((Mask >> (8 * Byte)) & 0xFF) == 0xFF)
          S.Stack[*Offset + Byte] = (Value >> (8 * Byte)) & 0xFF;
        else
          S.Stack.erase(*Offset + Byte);
      }
    } else if (not isRipRelative(M)) {
      // It could alias the stack
      S.Stack.clear();
    }
  }

  std::pair<uint64_t, uint64_t> read(const Operand &O, unsigned Bits) const {
    switch (O.K) {
    case Operand::Immediate:
      return { uint64_t(O.Imm) & widthMask(Bits), widthMask(Bits) };
    case Operand::Register:
      return readRegister(O.Reg);
    case Operand::Memory:
      return readMemory(O, Bits);
    }
    return { 0, 0 };
  }

  void write(const Operand &O, unsigned Bits, uint64_t Value, uint64_t Mask) {
    if (O.K == Operand::Register)
      writeRegister(O.Reg, Value, Mask);
    else if (O.K == Operand::Memory)
      writeMemory(O, Bits, Value, Mask);
  }

  /// The accumulator register of a width
  unsigned accumulator(unsigned Bits) const {
    StringRef Names[] = { "AL", "AX", "EAX", "RAX" };
    unsigned Index = Bits == 8 ? 0 : Bits == 16 ? 1 : Bits == 32 ? 2 : 3;
    for (auto &[Reg, Slice] : D.Slices)
      if (D.RegisterInfo.getName(Reg) == Names[Index])
        return Reg;
    return 0;
  }

  // Flags

  void resultFlags(uint64_t Result, uint64_t Mask, unsigned Bits) {
    uint64_t Width = widthMask(Bits);
    if (Mask & Result & Width)
      setFlag(ZF, false);
    else if ((Mask & Width) == Width)
      setFlag(ZF, (Result & Width) == 0);
    else
      setFlag(ZF, std::nullopt);
    if ((Mask >> (Bits - 1)) & 1)
      setFlag(SF, bool((Result >> (Bits - 1)) & 1));
    else
      setFlag(SF, std::nullopt);
    if ((Mask & 0xFF) == 0xFF)
      setFlag(PF, evenParity(Result));
    else
      setFlag(PF, std::nullopt);
  }

  void allFlagsUnknown() {
    for (unsigned Flag = 0; Flag < FlagCount; ++Flag)
      setFlag(Flag, std::nullopt);
  }

  void setFlag(unsigned Flag, Tri Value) {
    S.Flags[Flag] = Value;
    Touched |= 1u << Flag;
  }

  /// Make unknown what an instruction this interpreter does not model
  /// writes: its explicit and implicit register definitions, the stack bytes
  /// it may store to, and every flag if it writes EFLAGS
  /// Forget every bit of the 64-bit register \p Reg is part of
  void forgetWholeRegister(unsigned Reg) {
    auto It = D.Slices.find(Reg);
    if (It != D.Slices.end())
      S.Registers.erase(It->second.Base);
  }

  /// Make unknown what an instruction this interpreter does not model writes.
  /// An explicit destination is written as its width says, so a 32-bit one
  /// zeroes the upper half, except that bsf and bsr of a zero source leave
  /// their destination as it was on some processors: its upper half is then
  /// known only where it was known to be zero. The registers LLVM lists as
  /// written implicitly are forgotten whole: it lists EAX for cdq and EDI and
  /// ESI for the string instructions, which write none of them as 32 bits.
  void unmodeled() {
    for (unsigned Index = 0; Index < Desc.getNumDefs(); ++Index) {
      if (not I.getOperand(Index).isReg())
        continue;
      unsigned Reg = I.getOperand(Index).getReg();
      auto It = D.Slices.find(Reg);
      if ((Name.Family == "BSF" or Name.Family == "BSR")
          and It != D.Slices.end() and It->second.Bits == 32) {
        auto Old = S.Registers.find(It->second.Base);
        uint64_t ZeroUpper = Old == S.Registers.end() ?
                               0 :
                               Old->second.second & ~Old->second.first
                                 & ~widthMask(32);
        if (ZeroUpper == 0)
          S.Registers.erase(It->second.Base);
        else
          S.Registers[It->second.Base] = { 0, ZeroUpper };
        continue;
      }
      forgetRegister(Reg);
    }
    bool WritesFlags = false;
    for (MCPhysReg Reg : Desc.implicit_defs()) {
      if (D.RegisterInfo.getName(Reg) == StringRef("EFLAGS"))
        WritesFlags = true;
      else if (Reg == D.RSP)
        S.StackOffset.reset();
      else
        forgetWholeRegister(Reg);
    }
    if (Desc.mayStore()) {
      bool Known = false;
      for (const Operand &O : Operands)
        if (O.K == Operand::Memory and stackAddress(O)) {
          // The width is unknown here: forget the slot's neighborhood
          int64_t Offset = *stackAddress(O);
          for (int64_t Byte = 0; Byte < 64; ++Byte)
            S.Stack.erase(Offset + Byte);
          Known = true;
        } else if (O.K == Operand::Memory and isRipRelative(O)) {
          Known = true;
        }
      if (not Known)
        S.Stack.clear();
    }
    if (WritesFlags)
      allFlagsUnknown();
  }

  // Families

  bool arithmetic();
  bool unary();
  bool shift();
};

bool DecidedBranches::Interpreter::arithmetic() {
  StringRef F = Name.Family;
  unsigned Bits = Name.Bits;
  uint64_t Width = widthMask(Bits);
  bool Accumulator = Name.Form.starts_with("i");
  bool Compare = F == "CMP" or F == "TEST";

  Operand Destination;
  Operand Source;
  if (Accumulator) {
    Destination.K = Operand::Register;
    Destination.Reg = accumulator(Bits);
    if (Operands.empty())
      return true;
    Source = Operands.back();
  } else {
    if (Operands.size() < 2) {
      unmodeled();
      return true;
    }
    Destination = Operands[0];
    Source = Operands[1];
  }

  auto [A, AMask] = read(Destination, Bits);
  auto [B, BMask] = read(Source, Bits);
  if ((F == "XOR" or F == "SUB" or F == "CMP")
      and Destination.K == Operand::Register
      and Source.K == Operand::Register and Destination.Reg == Source.Reg) {
    A = B = 0;
    AMask = BMask = Width;
  }

  if (F == "AND" or F == "OR" or F == "XOR" or F == "TEST") {
    uint64_t Result = 0;
    uint64_t Mask = 0;
    if (F == "AND" or F == "TEST") {
      Result = A & B;
      Mask = (AMask & BMask) | (AMask & ~A) | (BMask & ~B);
    } else if (F == "OR") {
      Result = A | B;
      Mask = (AMask & BMask) | (AMask & A) | (BMask & B);
    } else {
      Result = A ^ B;
      Mask = AMask & BMask;
    }
    Mask &= Width;
    Result &= Mask;
    setFlag(CF, false);
    setFlag(OF, false);
    resultFlags(Result, Mask, Bits);
    if (not Compare)
      write(Destination, Bits, Result, Mask);
    return true;
  }

  uint64_t CarryIn = 0;
  if (F == "ADC" or F == "SBB") {
    if (not S.Flags[CF]) {
      unmodeled();
      return true;
    }
    CarryIn = *S.Flags[CF];
  }

  if ((AMask & Width) != Width or (BMask & Width) != Width) {
    if (not Compare)
      write(Destination, Bits, 0, 0);
    allFlagsUnknown();
    return true;
  }

  A &= Width;
  B &= Width;
  uint64_t Result = 0;
  if (F == "ADD" or F == "ADC") {
    unsigned __int128 Total = (unsigned __int128) A + B + CarryIn;
    Result = uint64_t(Total) & Width;
    setFlag(CF, Total > Width);
    setFlag(OF, ((~(A ^ B) & (A ^ Result)) >> (Bits - 1)) & 1);
  } else {
    unsigned __int128 Subtrahend = (unsigned __int128) B + CarryIn;
    Result = (A - B - CarryIn) & Width;
    setFlag(CF, Subtrahend > A);
    setFlag(OF, (((A ^ B) & (A ^ Result)) >> (Bits - 1)) & 1);
  }
  resultFlags(Result, Width, Bits);
  if (not Compare)
    write(Destination, Bits, Result, Width);
  return true;
}

bool DecidedBranches::Interpreter::unary() {
  StringRef F = Name.Family;
  unsigned Bits = Name.Bits;
  uint64_t Width = widthMask(Bits);
  if (Operands.empty()) {
    unmodeled();
    return true;
  }
  const Operand &Target = Operands[0];
  auto [A, AMask] = read(Target, Bits);
  if (F == "NOT") {
    write(Target, Bits, ~A & Width, AMask);
    return true;
  }
  if ((AMask & Width) != Width) {
    write(Target, Bits, 0, 0);
    if (F == "NEG") {
      allFlagsUnknown();
    } else {
      // inc and dec keep the carry
      setFlag(ZF, std::nullopt);
      setFlag(SF, std::nullopt);
      setFlag(PF, std::nullopt);
      setFlag(OF, std::nullopt);
    }
    return true;
  }
  uint64_t SignBit = uint64_t(1) << (Bits - 1);
  uint64_t Result = 0;
  if (F == "INC") {
    Result = (A + 1) & Width;
    setFlag(OF, Result == SignBit);
  } else if (F == "DEC") {
    Result = (A - 1) & Width;
    setFlag(OF, A == SignBit);
  } else {
    Result = (-A) & Width;
    setFlag(CF, A != 0);
    setFlag(OF, A == SignBit);
  }
  resultFlags(Result, Width, Bits);
  write(Target, Bits, Result, Width);
  return true;
}

bool DecidedBranches::Interpreter::shift() {
  StringRef F = Name.Family;
  unsigned Bits = Name.Bits;
  uint64_t Width = widthMask(Bits);
  if (Operands.empty()) {
    unmodeled();
    return true;
  }

  // The count: an immediate, 1 for the `r1` and `m1` forms, or CL when the
  // bits of it the instruction uses are known
  uint64_t CountMask = Bits == 64 ? 0x3F : 0x1F;
  std::optional<uint64_t> Count;
  if (Name.Form.ends_with("1")) {
    Count = 1;
  } else if (Name.Form.ends_with("CL")) {
    Operand Counter;
    Counter.Reg = D.CL;
    auto [Value, Mask] = read(Counter, 8);
    if ((Mask & CountMask) == CountMask)
      Count = Value;
  } else if (Operands.size() >= 2 and Operands.back().K == Operand::Immediate) {
    Count = uint64_t(Operands.back().Imm);
  }
  if (not Count) {
    // A rotate by any count changes only the carry and overflow flags
    if (F == "ROL" or F == "ROR" or F == "RCL" or F == "RCR") {
      write(Operands[0], Bits, 0, 0);
      setFlag(CF, std::nullopt);
      setFlag(OF, std::nullopt);
      return true;
    }
    unmodeled();
    return true;
  }
  *Count &= CountMask;
  if (*Count == 0)
    return true;

  const Operand &Target = Operands[0];
  auto [A, AMask] = read(Target, Bits);
  A &= Width;
  AMask &= Width;

  if (F == "RCL" or F == "RCR") {
    // Carry and overflow change, the other flags are kept
    write(Target, Bits, 0, 0);
    setFlag(CF, std::nullopt);
    setFlag(OF, std::nullopt);
    return true;
  }

  if (F == "ROL" or F == "ROR") {
    unsigned C = *Count % Bits;
    uint64_t Result = A;
    uint64_t Mask = AMask;
    if (C != 0) {
      if (F == "ROL") {
        Result = ((A << C) | (A >> (Bits - C))) & Width;
        Mask = ((AMask << C) | (AMask >> (Bits - C))) & Width;
      } else {
        Result = ((A >> C) | (A << (Bits - C))) & Width;
        Mask = ((AMask >> C) | (AMask << (Bits - C))) & Width;
      }
    }
    unsigned CarryBit = F == "ROL" ? 0 : Bits - 1;
    if ((Mask >> CarryBit) & 1)
      setFlag(CF, bool((Result >> CarryBit) & 1));
    else
      setFlag(CF, std::nullopt);
    setFlag(OF, std::nullopt);
    write(Target, Bits, Result, Mask);
    return true;
  }

  if (AMask != Width) {
    write(Target, Bits, 0, 0);
    allFlagsUnknown();
    return true;
  }
  uint64_t C = *Count;
  uint64_t Result = 0;
  bool Carry = false;
  if (F == "SHL" or F == "SAL") {
    Result = C >= 64 ? 0 : (A << C) & Width;
    Carry = C <= Bits ? (A >> (Bits - C)) & 1 : false;
  } else if (F == "SHR") {
    Result = A >> C;
    Carry = (A >> (C - 1)) & 1;
  } else {
    int64_t Signed = int64_t(A << (64 - Bits)) >> (64 - Bits);
    Result = uint64_t(Signed >> std::min<uint64_t>(C, 63)) & Width;
    Carry = (Signed >> std::min<uint64_t>(C - 1, 63)) & 1;
  }
  setFlag(CF, Carry);
  setFlag(OF, std::nullopt);
  resultFlags(Result, Width, Bits);
  write(Target, Bits, Result, Width);
  return true;
}

bool DecidedBranches::Interpreter::run() {
  StringRef F = Name.Family;
  StringRef Opcode = D.InstrInfo.getName(I.getOpcode());

  // Exceptions end the path: int3, icebp (int1), any int, such as the
  // `int 0x29` of __fastfail, ud2 and hlt
  if (Desc.isReturn() or Opcode == "INT3" or Opcode == "INT1"
      or Opcode == "TRAP" or Opcode == "HLT" or Opcode == "INT"
      or Opcode.starts_with("UD"))
    return false;

  // A write of the 32-bit ESP, which no real path does
  for (unsigned Index = 0; Index < Desc.getNumDefs(); ++Index)
    if (I.getOperand(Index).isReg()
        and D.RegisterInfo.getName(I.getOperand(Index).getReg())
              == StringRef("ESP"))
      return false;

  if (Desc.isCall()) {
    for (StringRef Volatile :
         { "RAX", "RCX", "RDX", "R8", "R9", "R10", "R11" })
      for (auto &[Reg, Slice] : D.Slices)
        if (D.RegisterInfo.getName(Reg) == Volatile)
          S.Registers.erase(Slice.Base);
    // The callee can write any slot whose address the function passed on, as
    // to an out-parameter
    S.Stack.clear();
    allFlagsUnknown();
    return true;
  }

  if (F == "PUSH" and not Operands.empty()) {
    auto [Value, Mask] = read(Operands[0], 64);
    if (Operands[0].K == Operand::Immediate)
      Mask = Full;
    if (S.StackOffset) {
      *S.StackOffset -= 8;
      Operand Slot;
      Slot.K = Operand::Memory;
      Slot.Base = D.RSP;
      writeMemory(Slot, 64, Value, Mask);
    }
    return true;
  }

  if (F == "POP" and not Operands.empty()) {
    if (S.StackOffset) {
      Operand Slot;
      Slot.K = Operand::Memory;
      Slot.Base = D.RSP;
      auto [Value, Mask] = readMemory(Slot, 64);
      int64_t After = *S.StackOffset + 8;
      write(Operands[0], 64, Value, Mask);
      if (not(Operands[0].K == Operand::Register
              and D.Slices.lookup(Operands[0].Reg).Base == D.RSP))
        S.StackOffset = After;
    } else {
      write(Operands[0], 64, 0, 0);
    }
    return true;
  }

  if ((F == "ADD" or F == "SUB") and Name.Bits == 64 and Operands.size() >= 2
      and Operands[0].K == Operand::Register and Operands[0].Reg == D.RSP
      and Operands[1].K == Operand::Immediate) {
    if (S.StackOffset)
      *S.StackOffset += F == "ADD" ? Operands[1].Imm : -Operands[1].Imm;
    allFlagsUnknown();
    return true;
  }

  if (Opcode == "CLC") {
    setFlag(CF, false);
    return true;
  }
  if (Opcode == "STC") {
    setFlag(CF, true);
    return true;
  }
  if (Opcode == "CMC") {
    setFlag(CF, triNot(S.Flags[CF]));
    return true;
  }

  if (F.starts_with("NOOP") or Opcode == "NOOP")
    return true;

  if (F == "MOV" and Operands.size() >= 2) {
    unsigned Bits = Name.Bits;
    auto [Value, Mask] = read(Operands[1], Bits);
    write(Operands[0], Bits, Value, Mask);
    return true;
  }

  if ((F == "MOVZX" or F == "MOVSX") and Operands.size() >= 2) {
    unsigned Bits = Name.Bits;
    unsigned SourceBits = 0;
    StringRef Digits = Name.Form.drop_while([](char C) {
      return not isdigit(C);
    });
    Digits.getAsInteger(10, SourceBits);
    if (SourceBits == 0 or SourceBits >= Bits) {
      unmodeled();
      return true;
    }
    auto [Value, Mask] = read(Operands[1], SourceBits);
    uint64_t High = widthMask(Bits) & ~widthMask(SourceBits);
    if (F == "MOVZX") {
      Mask |= High;
    } else if ((Mask >> (SourceBits - 1)) & 1) {
      if ((Value >> (SourceBits - 1)) & 1)
        Value |= High;
      Mask |= High;
    }
    write(Operands[0], Bits, Value, Mask);
    return true;
  }

  if (F == "XCHG" and Operands.size() >= 2) {
    unsigned Bits = Name.Bits;
    auto A = read(Operands[0], Bits);
    auto B = read(Operands[1], Bits);
    write(Operands[0], Bits, B.first, B.second);
    write(Operands[1], Bits, A.first, A.second);
    return true;
  }

  if (F == "LEA" and Operands.size() >= 2
      and Operands[1].K == Operand::Memory) {
    const Operand &M = Operands[1];
    unsigned Bits = Name.Form.starts_with("_32") ? 32 : Name.Bits;
    uint64_t Value = uint64_t(M.Displacement);
    uint64_t Mask = Full;
    if (isRipRelative(M)) {
      Value += Address + Size;
    } else if (M.Base != 0) {
      auto [BV, BM] = readRegister(M.Base);
      Value += BV;
      if (BM != Full)
        Mask = 0;
    }
    if (M.Index != 0) {
      auto [IV, IM] = readRegister(M.Index);
      Value += IV * M.Scale;
      if (IM != Full)
        Mask = 0;
    }
    write(Operands[0], Bits, Value, Mask);
    return true;
  }

  if (F == "ADD" or F == "SUB" or F == "CMP" or F == "ADC" or F == "SBB"
      or F == "AND" or F == "OR" or F == "XOR" or F == "TEST")
    return Name.Bits != 0 ? arithmetic() : (unmodeled(), true);

  if (F == "INC" or F == "DEC" or F == "NEG" or F == "NOT")
    return Name.Bits != 0 ? unary() : (unmodeled(), true);

  if (F == "SHL" or F == "SAL" or F == "SHR" or F == "SAR" or F == "ROL"
      or F == "ROR" or F == "RCL" or F == "RCR")
    return Name.Bits != 0 ? shift() : (unmodeled(), true);

  if (F == "BT") {
    // bt sets the carry; zero is kept and the others are undefined
    unmodeled();
    return true;
  }

  unmodeled();
  return true;
}

/// Decode the instruction at \p Code. LLVM rejects the shift group's
/// undocumented /6 encoding, an alias of shl (/4) that the protector uses, as
/// in `c0 f0 00`, `sal al, 0`: decode it as shl, which has the same length
/// and semantics.
static bool decode(const MCDisassembler &Disassembler,
                   MCInst &Instruction,
                   uint64_t &Size,
                   ArrayRef<uint8_t> Code,
                   uint64_t Address) {
  if (Disassembler.getInstruction(Instruction, Size, Code, Address, nulls())
        == MCDisassembler::Success
      and Size != 0)
    return true;

  SmallVector<uint8_t, 16> Copy(Code.take_front(15).begin(),
                                Code.take_front(15).end());
  size_t I = 0;
  auto IsPrefix = [](uint8_t Byte) {
    return Byte == 0x66 or Byte == 0x67 or Byte == 0xf0 or Byte == 0xf2
           or Byte == 0xf3 or Byte == 0x2e or Byte == 0x36 or Byte == 0x3e
           or Byte == 0x26 or Byte == 0x64 or Byte == 0x65;
  };
  while (I < Copy.size() and IsPrefix(Copy[I]))
    ++I;
  if (I < Copy.size() and (Copy[I] & 0xf0) == 0x40)
    ++I;
  if (I + 1 >= Copy.size())
    return false;
  uint8_t Opcode = Copy[I];
  uint8_t &ModRM = Copy[I + 1];
  bool ShiftGroup = Opcode == 0xc0 or Opcode == 0xc1 or (Opcode >= 0xd0
                                                          and Opcode <= 0xd3);
  if (not ShiftGroup or ((ModRM >> 3) & 7) != 6)
    return false;
  ModRM = (ModRM & ~0x38) | (4 << 3);
  return Disassembler.getInstruction(Instruction, Size, Copy, Address, nulls())
           == MCDisassembler::Success
         and Size != 0;
}

bool DecidedBranches::runsIntoInvalidInstruction(uint64_t Address) const {
  // A compiler writes a deliberate trap as `ud2` or `int3`, which decode and
  // end the search; a block longer than this ends at a branch long before
  constexpr unsigned MaxInstructions = 64;
  for (unsigned I = 0; I < MaxInstructions; ++I) {
    ArrayRef<uint8_t> Code = Bytes(Address);
    if (Code.empty())
      return false;

    MCInst Instruction;
    uint64_t Size = 0;
    if (not decode(Disassembler, Instruction, Size, Code, Address))
      return true;

    const MCInstrDesc &Desc = InstrInfo.get(Instruction.getOpcode());
    StringRef Name = InstrInfo.getName(Instruction.getOpcode());
    if (Desc.mayAffectControlFlow(Instruction, RegisterInfo) or Desc.isTrap()
        or Desc.isBarrier() or Name.starts_with("INT"))
      return false;

    Address += Size;
  }
  return false;
}

DecidedBranches::Decisions
DecidedBranches::walk(uint64_t Entry,
                      ArrayRef<std::pair<uint64_t, uint64_t>> Parts,
                      Coverage *Covered) const {
  if (Covered != nullptr)
    *Covered = Coverage();
  auto Inside = [&](uint64_t A) {
    for (auto [Begin, End] : Parts)
      if (Begin <= A and A < End)
        return true;
    return false;
  };

  // A state as "F=flags D=facts R=reg:value:mask,... P=rsp offset S=offset:byte,...",
  // each flag and fact 0, 1 or - when unknown
  auto Describe = [&](const State &S) {
    std::string Text = "F=";
    for (Tri Flag : S.Flags)
      Text += Flag ? (*Flag ? '1' : '0') : '-';
    Text += " D=";
    for (Tri Fact : S.Facts)
      Text += Fact ? (*Fact ? '1' : '0') : '-';
    Text += " R=";
    for (auto &[Register, Known] : S.Registers)
      Text += StringRef(RegisterInfo.getName(Register)).lower() + ":"
              + Twine::utohexstr(Known.first).str() + ":"
              + Twine::utohexstr(Known.second).str() + ",";
    Text += " P=";
    Text += S.StackOffset ? std::to_string(*S.StackOffset) : "?";
    Text += " S=";
    for (auto &[Offset, Byte] : S.Stack)
      Text += std::to_string(Offset) + ":" + std::to_string(Byte) + ",";
    return Text;
  };
  auto Hex = [](uint64_t A) { return "0x" + Twine::utohexstr(A).str(); };

  Decisions Decided;
  bool Incomplete = false;
  std::set<uint64_t> Undecided;
  std::set<uint64_t> Contradicted;
  std::map<uint64_t, State> Seen;
  std::vector<std::pair<uint64_t, State>> Work;
  Work.push_back({ Entry, State() });

  // A function whose walk takes this many paths is left undecided rather than
  // walked without bound
  constexpr unsigned MaxPaths = 200000;
  unsigned Paths = 0;
  while (not Work.empty()) {
    if (++Paths > MaxPaths) {
      revng_log(Log,
                "The walk from 0x" << Twine::utohexstr(Entry).str()
                                   << " took more than " << MaxPaths
                                   << " paths");
      if (Covered != nullptr)
        Covered->Starts.clear();
      return {};
    }
    auto [Address, S] = std::move(Work.back());
    Work.pop_back();
    if (not Inside(Address)) {
      if (Covered != nullptr)
        Covered->Exits.insert(Address);
      continue;
    }
    if (auto It = Seen.find(Address); It != Seen.end()) {
      State Joined = It->second.join(S);
      if (Joined == It->second)
        continue;
      S = std::move(Joined);
    }
    Seen[Address] = S;

    while (Inside(Address)) {
      ArrayRef<uint8_t> Code = Bytes(Address);
      if (Code.empty())
        break;
      MCInst Instruction;
      uint64_t Size = 0;
      if (not decode(Disassembler, Instruction, Size, Code, Address))
        break;
      if (Covered != nullptr)
        Covered->Starts.insert(Address);

      StringRef Name = InstrInfo.getName(Instruction.getOpcode());
      const MCInstrDesc &Desc = InstrInfo.get(Instruction.getOpcode());

      if (Name.starts_with("JCC_")) {
        uint64_t Target = Address + Size + Instruction.getOperand(0).getImm();
        unsigned Code = Instruction.getOperand(1).getImm() & 0xF;
        uint64_t Fallthrough = Address + Size;
        Tri Outcome = Conditions[Code].Outcome(S);
        if (Target == Fallthrough) {
          // Both outcomes lead to the next instruction: no successor is dead
          Work.push_back({ Fallthrough, S });
        } else if (Outcome) {
          revng_log(StatesLog,
                    "decide " << Hex(Address) << " " << (*Outcome ? '+' : '-')
                              << " " << Describe(S));
          auto It = Decided.find(Address);
          if (It != Decided.end() and It->second.Taken != *Outcome)
            Contradicted.insert(Address);
          Decided[Address] = { *Outcome, *Outcome ? Fallthrough : Target };
          Work.push_back({ *Outcome ? Target : Fallthrough, S });
        } else {
          Undecided.insert(Address);
          for (bool Taken : { true, false })
            revng_log(StatesLog,
                      "arm " << Hex(Address) << " " << (Taken ? '+' : '-')
                             << " " << Describe(S) << " | "
                             << Describe(implied(S, Code, Taken)));
          Work.push_back({ Target, implied(S, Code, true) });
          Work.push_back({ Fallthrough, implied(S, Code, false) });
        }
        break;
      }

      if (Desc.isBranch() and not Desc.isCall()) {
        if (Desc.isUnconditionalBranch() and not Desc.isIndirectBranch()
            and Instruction.getOperand(0).isImm()) {
          Work.push_back({ Address + Size + Instruction.getOperand(0).getImm(),
                           S });
        } else if (Desc.isIndirectBranch()) {
          // A jump through memory reads a function pointer, as from an
          // import slot or a vtable: a tail call. So is a jump through a
          // register with a REX.W prefix, which does not change it and which
          // MSVC writes only to end an epilogue, so that the x64 unwinder
          // recognises the jump as leaving the function. A switch jumps
          // through a register, or through memory indexed by a register, as
          // in `jmp [table + rcx * 8]`, whose destinations are unknown
          bool ThroughMemory = (Name == "JMP64m" or Name == "JMP32m")
                               and Instruction.getOperand(2).getReg() == 0;
          bool RexWRegister = Name == "JMP64r" and Size >= 3
                              and (Code[Size - 3] & 0xF8) == 0x48;
          bool TailCall = ThroughMemory or RexWRegister;
          std::optional<std::vector<uint64_t>> Targets;
          if (Indirect and not TailCall)
            Targets = Indirect(Address);
          if (Targets)
            for (uint64_t Target : *Targets)
              Work.push_back({ Target, S });
          else if (not TailCall) {
            revng_log(Log,
                      "The destinations of the jump at 0x"
                        << Twine::utohexstr(Address).str() << " are unknown");
            Incomplete = true;
          }
        }
        break;
      }

      // Nothing after a call to a function that does not return runs
      if (Desc.isCall() and NoReturn and Instruction.getOperand(0).isImm()
          and NoReturn(Address + Size + Instruction.getOperand(0).getImm()))
        break;

      std::optional<State> Before;
      if (StatesLog.isEnabled())
        Before = S;
      Interpreter Step(*this, Instruction, Address, Size, S);
      if (not Step.run())
        break;

      // The compound facts are kept while their flags are, and recomputed
      // from them otherwise
      if (Step.touched(CF) or Step.touched(ZF))
        S.Facts[BE] = triOr(S.Flags[CF], S.Flags[ZF]);
      if (Step.touched(SF) or Step.touched(OF))
        S.Facts[L] = triNe(S.Flags[SF], S.Flags[OF]);
      if (Step.touched(ZF) or Step.touched(SF) or Step.touched(OF))
        S.Facts[LE] = triOr(S.Flags[ZF], S.Facts[L]);

      if (Before)
        revng_log(StatesLog,
                  "step " << Hex(Address) << " " << Describe(*Before) << " | "
                          << Describe(S));
      Address += Size;
    }
  }

  // Paths the walk could not follow could reach any jump with other flags
  if (Incomplete) {
    if (Covered != nullptr)
      Covered->Starts.clear();
    return {};
  }
  if (Covered != nullptr)
    Covered->Complete = true;

  for (uint64_t Address : Undecided)
    Decided.erase(Address);
  for (uint64_t Address : Contradicted)
    Decided.erase(Address);
  return Decided;
}
