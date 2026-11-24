#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

namespace llvm {
class MCDisassembler;
class MCInst;
class MCInstrInfo;
class MCRegisterInfo;
} // namespace llvm

/// The conditional jumps of an x86-64 function that the values on its own
/// paths decide, such as the junk branches of a protector that a constant or
/// an earlier branch settles: `clc; jae` is always taken, and the fallthrough
/// of `jg X` takes a `jle Y` after it.
///
/// The function is walked from its entry by recursive descent within its
/// parts. The walk tracks the carry, zero, sign, parity and overflow flags and
/// three facts of conditions on them (below or equal, less, less or equal),
/// each known or unknown; the general registers as known bits; and the stack,
/// as the offset of rsp from its value at entry and the bytes stored at known
/// offsets from it. A call clobbers the volatile registers and the flags, a
/// write through an address the walk does not know forgets the stack's bytes,
/// and a path ends at a write of the 32-bit ESP, which no real path does. Each
/// arm of a conditional jump learns what its outcome implies. A jump counts as
/// decided only when every path that reaches it decides it the same way, and
/// only when its target is not the next instruction, which both outcomes
/// reach. An indirect jump continues at the destinations IndirectTargets
/// gives; a jump through memory, such as an import slot or a vtable entry, or
/// through a register with a REX.W prefix, which MSVC writes only to end an
/// epilogue, is a tail call and ends the path; and any other jump through a
/// register leaves paths unwalked, so no jump of the function counts as
/// decided. A path also ends at a direct call to a function that NoReturn says
/// does not return.
class DecidedBranches {
public:
  /// The bytes from an address on, empty when there are none
  using BytesAt = std::function<llvm::ArrayRef<uint8_t>(uint64_t)>;

  /// The destinations of the indirect jump at an address, such as a switch's
  /// through its table, when known
  using IndirectTargets =
    std::function<std::optional<std::vector<uint64_t>>(uint64_t)>;

  /// Whether the function at an address does not return
  using NoReturnCallee = std::function<bool(uint64_t)>;

  /// A decided jump: its outcome and the successor no path takes
  struct Decision {
    bool Taken = false;
    uint64_t DeadSuccessor = 0;
    bool operator==(const Decision &) const = default;
  };

  /// The decided jumps, by address
  using Decisions = std::map<uint64_t, Decision>;

  /// The code a walk reaches
  struct Coverage {
    /// Every path was followed to its end: no code of the function outside
    /// Starts runs when it is entered at its entry
    bool Complete = false;
    /// The starts of the instructions the paths reach, when Complete
    std::set<uint64_t> Starts;
    /// The addresses outside the function's parts that its paths continue
    /// at, such as the destination of a tail jump
    std::set<uint64_t> Exits;
  };

private:
  const llvm::MCDisassembler &Disassembler;
  const llvm::MCInstrInfo &InstrInfo;
  const llvm::MCRegisterInfo &RegisterInfo;
  BytesAt Bytes;
  IndirectTargets Indirect;
  NoReturnCallee NoReturn;

  /// For each LLVM register, its 64-bit register, the offset of its bits in
  /// it and their number; registers not listed are not tracked
  struct RegisterSlice {
    unsigned Base = 0;
    unsigned Shift = 0;
    unsigned Bits = 0;
  };
  llvm::DenseMap<unsigned, RegisterSlice> Slices;
  unsigned RSP = 0;
  /// The count register of shifts and rotates by CL
  unsigned CL = 0;

public:
  /// What a path knows: the flags, the facts, registers, the stack
  struct State;

  DecidedBranches(const llvm::MCDisassembler &Disassembler,
                  const llvm::MCInstrInfo &InstrInfo,
                  const llvm::MCRegisterInfo &RegisterInfo,
                  BytesAt Bytes,
                  IndirectTargets Indirect = {},
                  NoReturnCallee NoReturn = {});

  /// The decided jumps of the function entered at \p Entry, whose code lies
  /// in \p Parts, half-open address ranges
  /// \p Covered, when given, receives the code the walk reaches.
  Decisions walk(uint64_t Entry,
                 llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Parts,
                 Coverage *Covered = nullptr) const;

  /// Whether the straight-line code from \p Address reaches an encoding
  /// x86-64 does not have, such as `aad` or `push cs`, before any branch,
  /// call, return or trap. No working program runs such code.
  bool runsIntoInvalidInstruction(uint64_t Address) const;

private:
  class Interpreter;
};
