#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include "revng/Lift/DecidedBranches.h"
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
/// information (`.eh_frame`, or `.pdata` in an x86-64 PE file), used to tell
/// the entries of a jump table from the data that follows it.
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
    /// From .pdata, the start of the function's first entry, which the
    /// unwind chain ends at
    uint64_t Primary = 0;
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
  /// The ranges come from a PE program's `.pdata`
  bool FromPData = false;
  /// For ranges from .pdata, the address the program's relative addresses
  /// are relative to
  uint64_t ImageBase = 0;
  MetaAddressType::Values CodeType = MetaAddressType::Invalid;

  /// Sorted by start
  std::vector<Range> Ranges;

  llvm::DenseMap<unsigned, Sweep> Sweeps;

  /// For each range, the ranges that start mid-frame and jump into it
  llvm::DenseMap<unsigned, llvm::SmallVector<unsigned, 1>> JumpsFromParts;
  bool PartsIndexed = false;

  llvm::DenseMap<unsigned, std::vector<unsigned>> Groups;
  /// The .pdata parts of each function, by the start of its first entry
  llvm::DenseMap<uint64_t, std::vector<unsigned>> PartsByPrimary;
  /// What the walk of a function found
  struct Walked {
    DecidedBranches::Decisions Decisions;
    /// Every path of the function was followed to its end
    bool Complete = false;
    /// The starts of the instructions its paths reach, sorted, when Complete
    std::vector<uint64_t> Starts;
  };

  /// Each function walked, by the range of its entry
  std::map<unsigned, Walked> WalkedByEntry;

  /// The gap code walked from roots: the starts of the instructions their
  /// paths reach, the extents of roots whose walk did not follow every path,
  /// the roots walked, and the gap addresses the paths of any walk continue
  /// at
  llvm::DenseSet<uint64_t> GapStarts;
  std::vector<std::pair<uint64_t, uint64_t>> GapExtents;
  llvm::DenseSet<uint64_t> GapRoots;
  llvm::DenseSet<uint64_t> GapExits;

  /// The callees of the calls that end a range followed only by nops,
  /// collected on first use
  std::optional<llvm::DenseSet<uint64_t>> NoReturnCallees;
  const llvm::DenseSet<uint64_t> &noReturnCallees();
  /// The destination of the direct call in range \p Index that ends right
  /// before \p Return, if there is one
  std::optional<uint64_t> directCallBefore(unsigned Index, uint64_t Return);

  DecidedBranches makeWalk();
  /// A walker kept for the checks that decode code one address at a time
  std::optional<DecidedBranches> Decoder;
  void recordExits(const DecidedBranches::Coverage &Covered);

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

  /// The start of every range that begins a function
  std::vector<MetaAddress> functionEntries() const;

  /// Whether Address lies in a PE program's unwind range other than at the
  /// start of a function: in the middle of a function or in a split-off part
  /// of one. Every PE function that is not a leaf has unwind information, and
  /// code does not call into the middle of another function, so no function
  /// starts there. The rule does not hold for `.eh_frame`, whose hand-written
  /// functions can have several entry points.
  bool isInsideFunction(MetaAddress Address) const;

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

  /// The destinations of an indirect jump at \p Jump that dispatches through
  /// a table of 32-bit offsets from the table's start, as compilers emit for a
  /// switch:
  ///
  ///     lea    TABLE(%rip), %B
  ///     movslq (%B,%I,4), %X
  ///     add    %B, %X
  ///     jmp    *%X
  ///
  /// The table is read entry by entry from its start and bounded as by
  /// tableExtent. This serves a jump whose index the analysis cannot bound,
  /// such as an enum discriminant the compiler knows to be in range. Returns
  /// std::nullopt when the instructions before \p Jump are not this pattern or
  /// no entry leads into the jump's function.
  std::optional<
      std::pair<llvm::SmallVector<MetaAddress>, llvm::SmallVector<uint64_t>>>
  relativeJumpTable(MetaAddress Jump);

  /// The successor of the conditional jump at \p Jump that no path of its
  /// function takes, when the values on those paths decide it: see
  /// DecidedBranches. x86-64 only.
  std::optional<MetaAddress> deadSuccessor(MetaAddress Jump);

  /// Whether \p Address lies in a function whose every path the walk of
  /// DecidedBranches follows, and no path reaches an instruction starting
  /// there: code that never runs, such as junk in obfuscated programs. x86-64
  /// only.
  bool isDeadCode(MetaAddress Address);

  /// Whether \p Address lies in a PE program's code outside every unwind
  /// range: in the leaf functions and padding between functions, or junk.
  bool isGapCode(MetaAddress Address) const;

  /// Whether \p Address, gap code, is reached from a root. A root is an
  /// address \p IsRoot says is, such as the target of a call or a pointer in
  /// data, or one a walk's path continues at, such as a tail jump's
  /// destination; it is walked up to the next unwind range. Other gap code
  /// counts only when such a walk reaches it. x86-64 only.
  bool isReachedGapCode(MetaAddress Address, bool IsRoot);

  /// Whether a walked path continues at \p Address, gap code
  bool isGapExit(MetaAddress Address) const {
    return GapExits.contains(Address.address());
  }

  /// Whether \p Address is where a direct call right before it would return
  /// to, and the callee does not return. A callee does not return when some
  /// function's unwind range ends with a call to it followed only by nops:
  /// MSVC ends a function so when the last thing it does is call a function
  /// that does not return, as the nops keep the return address in the
  /// caller's range for the unwinder. x86-64 PE only.
  bool isNoReturnCallReturn(MetaAddress Address);

  /// Whether the straight-line code from \p Address reaches an encoding x86-64
  /// does not have before any branch: see
  /// DecidedBranches::runsIntoInvalidInstruction. x86-64 only.
  bool runsIntoInvalidInstruction(MetaAddress Address);

private:
  std::optional<unsigned> find(uint64_t Address) const;
  const Sweep &sweep(unsigned Index);
  const std::vector<unsigned> &group(unsigned Index);
  /// The walk of the function containing the range \p Index
  const Walked &walked(unsigned Index);
  void indexParts();
  bool isInstructionStart(unsigned Index, uint64_t Address);
  bool initializeDisassembler(model::Architecture::Values Architecture);
};
