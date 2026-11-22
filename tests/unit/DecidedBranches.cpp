//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DecidedBranches
bool init_unit_test();
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"

#include "boost/test/unit_test.hpp"

#include "revng/Lift/DecidedBranches.h"

using namespace llvm;

namespace {

/// An x86-64 decoder, and code placed at 0x1000 as one function
class Fixture {
  std::unique_ptr<MCRegisterInfo> RegisterInfo;
  std::unique_ptr<MCAsmInfo> AsmInfo;
  std::unique_ptr<MCSubtargetInfo> SubtargetInfo;
  std::unique_ptr<MCInstrInfo> InstrInfo;
  std::unique_ptr<MCContext> Context;
  std::unique_ptr<MCDisassembler> Disassembler;

public:
  Fixture() {
    InitializeAllTargetInfos();
    InitializeAllTargetMCs();
    InitializeAllDisassemblers();
    std::string Error;
    std::string TripleName = "x86_64";
    const Target *TheTarget = TargetRegistry::lookupTarget(TripleName, Error);
    BOOST_REQUIRE(TheTarget != nullptr);
    RegisterInfo.reset(TheTarget->createMCRegInfo(TripleName));
    MCTargetOptions Options;
    AsmInfo.reset(TheTarget->createMCAsmInfo(*RegisterInfo,
                                             TripleName,
                                             Options));
    SubtargetInfo.reset(TheTarget->createMCSubtargetInfo(TripleName, "", ""));
    InstrInfo.reset(TheTarget->createMCInstrInfo());
    Context = std::make_unique<MCContext>(Triple(TripleName),
                                          AsmInfo.get(),
                                          RegisterInfo.get(),
                                          SubtargetInfo.get());
    Disassembler.reset(TheTarget->createMCDisassembler(*SubtargetInfo,
                                                       *Context));
  }

  /// The decided jumps of \p Code, by offset from its start
  std::map<uint64_t, bool> decided(std::vector<uint8_t> Code) {
    Code.insert(Code.end(), 16, 0xcc);
    DecidedBranches Walk(*Disassembler,
                         *InstrInfo,
                         *RegisterInfo,
                         [&Code](uint64_t Address) -> ArrayRef<uint8_t> {
                           if (Address < 0x1000
                               or Address >= 0x1000 + Code.size())
                             return {};
                           return ArrayRef(Code).drop_front(Address - 0x1000);
                         });
    std::pair<uint64_t, uint64_t> Part{ 0x1000, 0x1000 + Code.size() };
    std::map<uint64_t, bool> Result;
    for (auto &[Address, D] : Walk.walk(0x1000, Part))
      Result[Address - 0x1000] = D.Taken;
    return Result;
  }

  /// Whether the walk of \p Code follows every path, and the offsets of the
  /// instructions its paths reach
  std::pair<bool, std::set<uint64_t>> coverage(std::vector<uint8_t> Code) {
    Code.insert(Code.end(), 16, 0xcc);
    DecidedBranches Walk(*Disassembler,
                         *InstrInfo,
                         *RegisterInfo,
                         [&Code](uint64_t Address) -> ArrayRef<uint8_t> {
                           if (Address < 0x1000
                               or Address >= 0x1000 + Code.size())
                             return {};
                           return ArrayRef(Code).drop_front(Address - 0x1000);
                         });
    std::pair<uint64_t, uint64_t> Part{ 0x1000, 0x1000 + Code.size() };
    DecidedBranches::Coverage Covered;
    Walk.walk(0x1000, Part, &Covered);
    std::set<uint64_t> Starts;
    for (uint64_t Address : Covered.Starts)
      Starts.insert(Address - 0x1000);
    return { Covered.Complete, Starts };
  }
};

using Decisions = std::map<uint64_t, bool>;

} // namespace

BOOST_AUTO_TEST_CASE(SetCarryDecidesBelowOrEqual) {
  // stc; jbe +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x76, 0x01, 0xcc, 0xc3 }) == Decisions{ { 1, true } }));
}

BOOST_AUTO_TEST_CASE(FallthroughOfGreaterDecidesLessOrEqual) {
  // inc eax; jg +3; jle +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xff, 0xc0, 0x7f, 0x03, 0x7e, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 4, true } }));
}

BOOST_AUTO_TEST_CASE(ShiftByZeroKeepsFacts) {
  // inc eax; jg +6; sal bl, 0; jle +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xff, 0xc0, 0x7f, 0x06, 0xc0, 0xe3, 0x00, 0x7e, 0x01,
                          0xcc, 0xc3 })
              == Decisions{ { 7, true } }));
}

BOOST_AUTO_TEST_CASE(RotateKeepsZero) {
  // test eax, eax; je +6; ror ebx, 3; jne +1; int3; ret
  // On the fallthrough of je, zero is clear, and ror leaves it so
  Fixture F;
  BOOST_TEST((F.decided({ 0x85, 0xc0, 0x74, 0x06, 0xc1, 0xcb, 0x03, 0x75, 0x01,
                          0xcc, 0xc3 })
              == Decisions{ { 7, true } }));
}

BOOST_AUTO_TEST_CASE(StackRoundTripAndRotate) {
  // mov dword [rsp+0x10], 0x4631b221; mov eax, [rsp+0x10]; ror eax, 0xc;
  // add eax, -0x68; jae +1; int3; ret
  // ror gives 0x2214631b, and adding -0x68 carries
  Fixture F;
  BOOST_TEST((F.decided({ 0xc7, 0x44, 0x24, 0x10, 0x21, 0xb2, 0x31, 0x46,
                          0x8b, 0x44, 0x24, 0x10, 0xc1, 0xc8, 0x0c, 0x83,
                          0xc0, 0x98, 0x73, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 18, false } }));
}

BOOST_AUTO_TEST_CASE(StackPointerWriteEndsThePath) {
  // cmp esp, 0x1000; ja +3; sub esp, 0x10; je +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0x81, 0xfc, 0x00, 0x10, 0x00, 0x00, 0x77, 0x03, 0x83,
                          0xec, 0x10, 0x74, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 11, false } }));
}

BOOST_AUTO_TEST_CASE(UndecidedOnOnePathIsNotDecided) {
  // test eax, eax; je +2; clc; nop; jae +1; int3; ret: carry is clear on
  // both paths
  Fixture F;
  BOOST_TEST((F.decided({ 0x85, 0xc0, 0x74, 0x02, 0xf8, 0x90, 0x73, 0x01, 0xcc,
                          0xc3 })
              == Decisions{ { 6, true } }));
  // With stc on one path the paths disagree
  BOOST_TEST((F.decided({ 0x85, 0xc0, 0x74, 0x02, 0xf9, 0x90, 0x73, 0x01, 0xcc,
                          0xc3 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(CallClobbersFlags) {
  // clc; call +0; jae +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xf8, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x73, 0x01, 0xcc,
                          0xc3 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(UnknownMemoryWriteForgetsTheStack) {
  // mov dword [rsp+0x10], 1; mov [rdx], eax; mov eax, [rsp+0x10];
  // test eax, eax; jne +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00,
                          0x89, 0x02, 0x8b, 0x44, 0x24, 0x10, 0x85, 0xc0,
                          0x75, 0x01, 0xcc, 0xc3 })
              == Decisions{}));
  // Without the write through rdx the value is known
  BOOST_TEST((F.decided({ 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00,
                          0x8b, 0x44, 0x24, 0x10, 0x85, 0xc0, 0x75, 0x01,
                          0xcc, 0xc3 })
              == Decisions{ { 14, true } }));
}

BOOST_AUTO_TEST_CASE(RotateByUnknownCountKeepsZero) {
  // xor eax, eax; ror edx, cl; jne +1; int3; ret
  // A rotate by any count changes only the carry and overflow flags
  Fixture F;
  BOOST_TEST((F.decided({ 0x31, 0xc0, 0xd3, 0xca, 0x75, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 4, false } }));
}

BOOST_AUTO_TEST_CASE(ShiftByKnownCl) {
  // mov eax, 3; mov ecx, 3; shl eax, cl; cmp eax, 0x18; je +1; int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xb8, 0x03, 0x00, 0x00, 0x00, 0xb9, 0x03,
                          0x00, 0x00, 0x00, 0xd3, 0xe0, 0x83, 0xf8,
                          0x18, 0x74, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 15, true } }));
}

BOOST_AUTO_TEST_CASE(UndocumentedShiftAlias) {
  // stc; sal al, 0 in its /6 encoding, which LLVM does not decode; jbe +1;
  // int3; ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0xc0, 0xf0, 0x00, 0x76, 0x01, 0xcc, 0xc3 })
              == Decisions{ { 4, true } }));
}

BOOST_AUTO_TEST_CASE(JumpToTheNextInstructionHasNoDeadSuccessor) {
  // stc; jb +0; ret: taken or not, the path continues at the ret
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x00, 0xc3 }) == Decisions{}));
}

BOOST_AUTO_TEST_CASE(UnknownIndirectJumpLeavesTheFunctionUndecided) {
  // stc; jb +1; int3; jmp rax: the jump's destinations are unknown, so paths
  // could reach jb with other flags
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0xff, 0xe0 }) == Decisions{}));
}

BOOST_AUTO_TEST_CASE(JumpThroughSlotIsATailCall) {
  // stc; jb +1; int3; jmp [rip + 0]: a tail call through an import slot
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0xff, 0x25, 0x00, 0x00, 0x00,
                          0x00 })
              == Decisions{ { 1, true } }));
}

BOOST_AUTO_TEST_CASE(JumpThroughVtableIsATailCall) {
  // stc; jb +1; int3; jmp [rax + 0x10]
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0x48, 0xff, 0x60, 0x10 })
              == Decisions{ { 1, true } }));
}

BOOST_AUTO_TEST_CASE(CdqKeepsTheUpperHalfOfRax) {
  // mov rax, -1; cdq; shr rax, 32; test rax, rax; je +1; int3; ret: LLVM
  // lists EAX among cdq's writes, but rax keeps its upper half, which is not
  // zero, so je is not decided taken
  Fixture F;
  BOOST_TEST((F.decided({ 0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x99, 0x48,
                          0xc1, 0xe8, 0x20, 0x48, 0x85, 0xc0, 0x74, 0x01, 0xcc,
                          0xc3 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(BitScanOfZeroMayKeepTheUpperHalf) {
  // bsr ecx, ecx; shr rcx, 32; test rcx, rcx; je +1; int3; ret: when ecx is
  // zero some processors leave rcx as it was, so its unknown upper half stays
  // unknown
  Fixture F;
  BOOST_TEST((F.decided({ 0x0f, 0xbd, 0xc9, 0x48, 0xc1, 0xe9, 0x20, 0x48, 0x85,
                          0xc9, 0x74, 0x01, 0xcc, 0xc3 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(LeaIntoA32BitRegister) {
  // mov ecx, 0xed961135; lea eax, [rcx + 0x3c232685]; cmp eax, 0x29b937ba;
  // jne +1; int3; ret: eax is the low half of the sum
  Fixture F;
  BOOST_TEST((F.decided({ 0xb9, 0x35, 0x11, 0x96, 0xed, 0x8d, 0x81, 0x85, 0x26,
                          0x23, 0x3c, 0x3d, 0xba, 0x37, 0xb9, 0x29, 0x75, 0x01,
                          0xcc, 0xc3 })
              == Decisions{ { 16, false } }));
}

BOOST_AUTO_TEST_CASE(RexWJumpThroughRegisterIsATailCall) {
  // stc; jb +1; int3; rex.WB jmp r9: MSVC's tail call at the end of an
  // epilogue; without REX.W, jmp r9 could be a switch
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0x49, 0xff, 0xe1 })
              == Decisions{ { 1, true } }));
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0x41, 0xff, 0xe1 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(IcebpEndsThePath) {
  // test eax, eax; je +3; int1; stc; nop; jb +1; int3; ret: past int1 nothing
  // runs, so the stc that would disagree with the carry test clears is not
  // reached, and jb is never taken
  Fixture F;
  BOOST_TEST((F.decided({ 0x85, 0xc0, 0x74, 0x03, 0xf1, 0xf9, 0x90, 0x72, 0x01,
                          0xcc, 0xc3 })
              == Decisions{ { 7, false } }));
}

BOOST_AUTO_TEST_CASE(CompareSameRegister) {
  // cmp eax, eax; je +1; nop; ret: comparing a register with itself sets ZF
  Fixture F;
  BOOST_TEST((F.decided({ 0x39, 0xc0, 0x74, 0x01, 0x90, 0xc3 })
              == Decisions{ { 2, true } }));
}

namespace {

struct FlowCleanCase {
  const char *Name;
  const char *Hex;
  Decisions Expected;
};

const FlowCleanCase FlowCleanFixtures[] = {
#include "DecidedBranchesFlowCleanCases.inc"
};

std::vector<uint8_t> hexBytes(llvm::StringRef Hex) {
  std::vector<uint8_t> Result;
  for (size_t I = 0; I + 1 < Hex.size(); I += 2)
    Result.push_back(std::stoi(Hex.substr(I, 2).str(), nullptr, 16));
  return Result;
}

} // namespace

BOOST_AUTO_TEST_CASE(FlowCleanCases) {
  Fixture F;
  for (const FlowCleanCase &Case : FlowCleanFixtures) {
    BOOST_TEST_CONTEXT(Case.Name) {
      BOOST_TEST((F.decided(hexBytes(Case.Hex)) == Case.Expected));
    }
  }
}

BOOST_AUTO_TEST_CASE(IndexedMemoryJumpIsUnknown) {
  // stc; jb +1; int3; jmp [rax * 8 + 0x1000]: a jump through memory indexed
  // by a register, as a switch's, has unknown destinations
  Fixture F;
  BOOST_TEST((F.decided({ 0xf9, 0x72, 0x01, 0xcc, 0xff, 0x24, 0xc5, 0x00, 0x10,
                          0x00, 0x00 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(CoverageIsTheLiveInstructions) {
  // stc; jb +1; int3; ret: the int3 is on no path
  Fixture F;
  auto [Complete, Starts] = F.coverage({ 0xf9, 0x72, 0x01, 0xcc, 0xc3 });
  BOOST_TEST(Complete);
  BOOST_TEST((Starts == std::set<uint64_t>{ 0, 1, 4 }));
}

BOOST_AUTO_TEST_CASE(CallForgetsTheStack) {
  // mov qword [rsp + 8], 0; call +0; cmp qword [rsp + 8], 0; je +1; int3; ret:
  // the callee may write the slot, whose address may have been passed on
  Fixture F;
  BOOST_TEST((F.decided({ 0x48, 0xc7, 0x44, 0x24, 0x08, 0x00, 0x00, 0x00, 0x00,
                          0xe8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0x7c, 0x24,
                          0x08, 0x00, 0x74, 0x01, 0xcc, 0xc3 })
              == Decisions{}));
}

BOOST_AUTO_TEST_CASE(ShortStackPointerWritesForgetTheStack) {
  // mov qword [rsp + 8], 0; WRITE; cmp qword [rsp + 8], 0; je +1; int3; ret:
  // a write to esp, as `xchg esp, eax` or `add esp, 0`, which zeroes rsp's
  // upper half, leaves rsp's offset from its value at entry unknown
  for (std::vector<uint8_t> Write : { std::vector<uint8_t>{ 0x94 },
                                      std::vector<uint8_t>{ 0x83, 0xc4, 0x00 },
                                      std::vector<uint8_t>{ 0xd3, 0xfc } }) {
    std::vector<uint8_t> Code = { 0x48, 0xc7, 0x44, 0x24, 0x08,
                                  0x00, 0x00, 0x00, 0x00 };
    Code.insert(Code.end(), Write.begin(), Write.end());
    for (uint8_t Byte : { 0x48, 0x83, 0x7c, 0x24, 0x08, 0x00, 0x74, 0x01, 0xcc,
                          0xc3 })
      Code.push_back(Byte);
    Fixture F;
    BOOST_TEST((F.decided(Code) == Decisions{}));
  }
}
