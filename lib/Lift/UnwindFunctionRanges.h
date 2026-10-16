#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include "revng/Model/Architecture.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/Support/MetaAddress.h"

namespace llvm {
class MCContext;
class MCDisassembler;
class MCInstrAnalysis;
class MCInstrInfo;
class MCRegisterInfo;
class MCAsmInfo;
class MCSubtargetInfo;
} // namespace llvm

/// The code ranges of the functions described by a binary's unwind
/// information (`.eh_frame`), used to tell the entries of a jump table from the
/// data that follows it.
///
/// A function the compiler split into a hot and one or more cold parts has one
/// range per part. Each part other than the entry begins with the frame of the
/// function already set up, so its unwind entry describes, before the first
/// instruction, a state different from the one at a call. Such a part and the
/// parts it is connected to by direct jumps form one function.
class UnwindFunctionRanges {
private:
  struct Range {
    uint64_t Start = 0;
    uint64_t End = 0;
    /// The unwind entry describes a frame already set up at Start: this is a
    /// part of a function entered by a jump, not a function of its own
    bool StartsMidFrame = false;
  };

  /// The decoding of one range from its start, one instruction after another
  struct Sweep {
    /// Bit I is set when Start + I is the start of a decoded instruction
    llvm::BitVector InstructionStarts;
    /// The address where decoding failed, or End: past it every address counts
    /// as an instruction start
    uint64_t DecodedEnd = 0;
    /// The destinations of the direct jumps (not calls) in the range
    llvm::SmallVector<uint64_t, 4> DirectJumps;
    /// The memory addresses the range's instructions compute from the program
    /// counter, such as the start of a jump table, sorted
    std::vector<uint64_t> DataReferences;
  };

private:
  const RawBinaryView &BinaryView;
  model::Architecture::Values Architecture;
  MetaAddressType::Values CodeType = MetaAddressType::Invalid;

  /// Sorted by start
  std::vector<Range> Ranges;

  llvm::DenseMap<unsigned, Sweep> Sweeps;

  /// For each range, the ranges that start mid-frame and jump into it
  llvm::DenseMap<unsigned, llvm::SmallVector<unsigned, 1>> JumpsFromParts;
  bool PartsIndexed = false;

  llvm::DenseMap<unsigned, std::vector<unsigned>> Groups;

  std::unique_ptr<llvm::MCRegisterInfo> RegisterInfo;
  std::unique_ptr<llvm::MCAsmInfo> AsmInfo;
  std::unique_ptr<llvm::MCSubtargetInfo> SubtargetInfo;
  std::unique_ptr<llvm::MCInstrInfo> InstrInfo;
  std::unique_ptr<llvm::MCContext> Context;
  std::unique_ptr<llvm::MCDisassembler> Disassembler;
  std::unique_ptr<llvm::MCInstrAnalysis> InstrAnalysis;

public:
  UnwindFunctionRanges(const RawBinaryView &BinaryView,
                       llvm::ArrayRef<uint8_t> File,
                       model::Architecture::Values Architecture);
  ~UnwindFunctionRanges();

public:
  bool empty() const { return Ranges.empty(); }
  size_t size() const { return Ranges.size(); }

  /// Whether decoding from an unwind range's start places Address inside an
  /// instruction. Missing unwind information or a failed decode is unknown,
  /// so returns false. This is a filter for speculative raw pointer scans,
  /// not a restriction on explicit entries or recovered branches.
  bool isInstructionInterior(MetaAddress Address);

  /// Whether Address is the start of an unwind range that begins a function:
  /// one whose unwind entry describes the state at a call, not a split-off part
  /// of another function entered by a jump.
  bool isFunctionEntry(MetaAddress Address) const;

  /// Return how many of \p Targets, the destinations an indirect jump at
  /// \p Jump reads from the table entries at \p Entries (in ascending address
  /// order, one per destination), belong to the table: the leading run of
  /// destinations that lie in the function containing \p Jump at the start of
  /// an instruction, read from entries before the next address after the
  /// first entry that the function's code refers to, which starts another
  /// table or object.
  ///
  /// \return std::nullopt when there is no unwind information for \p Jump.
  std::optional<size_t> tableExtent(MetaAddress Jump,
                                    llvm::ArrayRef<MetaAddress> Targets,
                                    llvm::ArrayRef<uint64_t> Entries);

private:
  std::optional<unsigned> find(uint64_t Address) const;
  const Sweep &sweep(unsigned Index);
  const std::vector<unsigned> &group(unsigned Index);
  void indexParts();
  bool isInstructionStart(unsigned Index, uint64_t Address);
  bool initializeDisassembler(model::Architecture::Values Architecture);
};
