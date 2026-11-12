#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

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
/// reach.
class DecidedBranches {
public:
  /// The bytes from an address on, empty when there are none
  using BytesAt = std::function<llvm::ArrayRef<uint8_t>(uint64_t)>;

  /// A decided jump: its outcome and the successor no path takes
  struct Decision {
    bool Taken = false;
    uint64_t DeadSuccessor = 0;
    bool operator==(const Decision &) const = default;
  };

  /// The decided jumps, by address
  using Decisions = std::map<uint64_t, Decision>;

private:
  const llvm::MCDisassembler &Disassembler;
  const llvm::MCInstrInfo &InstrInfo;
  const llvm::MCRegisterInfo &RegisterInfo;
  BytesAt Bytes;

  /// For each LLVM register, its 64-bit register, the offset of its bits in
  /// it and their number; registers not listed are not tracked
  struct RegisterSlice {
    unsigned Base = 0;
    unsigned Shift = 0;
    unsigned Bits = 0;
  };
  llvm::DenseMap<unsigned, RegisterSlice> Slices;
  unsigned RSP = 0;

public:
  /// What a path knows: the flags, the facts, registers, the stack
  struct State;

  DecidedBranches(const llvm::MCDisassembler &Disassembler,
                  const llvm::MCInstrInfo &InstrInfo,
                  const llvm::MCRegisterInfo &RegisterInfo,
                  BytesAt Bytes);

  /// The decided jumps of the function entered at \p Entry, whose code lies
  /// in \p Parts, half-open address ranges
  Decisions
  walk(uint64_t Entry,
       llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Parts) const;

private:
  class Interpreter;
};
