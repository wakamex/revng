/// This file handles the whole translation process from the input assembly to
/// LLVM IR.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Progress.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "revng/ABI/ModelHelpers.h"
#include "revng/ADT/Queue.h"
#include "revng/ADT/STLExtras.h"
#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/FunctionCallIdentification/FunctionCallIdentification.h"
#include "revng/FunctionCallIdentification/PruneRetSuccessors.h"
#include "revng/Lift/Helpers.h"
#include "revng/Lift/VariableManager.h"
#include "revng/Model/Architecture.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/Importer/DebugInfo/DwarfImporter.h"
#include "revng/Model/NameBuilder.h"
#include "revng/Model/ProgramCounterHandler.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/Support/CommandLine.h"
#include "revng/Support/Debug.h"
#include "revng/Support/EmitAbort.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelper.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/SimplePassManager.h"

#include "CodeGenerator.h"
#include "ExternalJumpsHandler.h"
#include "InstructionTranslator.h"
#include "JumpTargetManager.h"

using namespace llvm;

using std::make_pair;
using std::string;

// Register all the arguments
static cl::opt<bool> RecordTCG("record-tcg",
                               cl::desc("create metadata for TCG"),
                               cl::cat(MainCategory));

static cl::opt<bool> PruneDecidedBranches("prune-decided-branches",
                                          cl::desc("Leave out the successor "
                                                   "of an x86-64 conditional "
                                                   "jump that no path of its "
                                                   "function takes, as the "
                                                   "junk branches a protector "
                                                   "settles with constants"),
                                          cl::init(false));

static Logger LibTcgLog("libtcg");
static Logger Log("lift");

/// Whether the x86-64 instruction in \p Bytes writes the 32-bit ESP, which
/// zeroes the upper half of RSP. Compiled code never does it, and a path that
/// does would continue with a stack real programs never have: Warcraft III's
/// protector guards code with `cmp esp, imm32; ja; sub esp, imm8`, whose
/// `sub esp` only junk paths reach. Recognizes the arithmetic, mov and lea
/// forms; a 16-bit write keeps the upper half and REX.W makes it a 64-bit one.
static bool writesTruncatedStackPointer(llvm::ArrayRef<uint8_t> Bytes) {
  size_t I = 0;
  bool OperandSize16 = false;
  for (; I < Bytes.size(); ++I) {
    uint8_t Prefix = Bytes[I];
    if (Prefix == 0x66)
      OperandSize16 = true;
    else if (Prefix != 0x67 and Prefix != 0xf0 and Prefix != 0xf2
             and Prefix != 0xf3 and Prefix != 0x2e and Prefix != 0x36
             and Prefix != 0x3e and Prefix != 0x26 and Prefix != 0x64
             and Prefix != 0x65)
      break;
  }

  uint8_t Rex = 0;
  if (I < Bytes.size() and (Bytes[I] & 0xf0) == 0x40)
    Rex = Bytes[I++];
  bool RexW = Rex & 8;
  bool RexR = Rex & 4;
  bool RexB = Rex & 1;
  if (OperandSize16 or RexW or I >= Bytes.size())
    return false;

  uint8_t Opcode = Bytes[I++];
  // mov esp, imm32
  if (Opcode == 0xbc)
    return not RexB;
  if (I >= Bytes.size())
    return false;

  uint8_t ModRM = Bytes[I];
  unsigned Mod = ModRM >> 6;
  unsigned Reg = (ModRM >> 3) & 7;
  unsigned Rm = ModRM & 7;
  bool RmIsESP = Mod == 3 and Rm == 4 and not RexB;
  bool RegIsESP = Reg == 4 and not RexR;

  switch (Opcode) {
  // add, or, adc, sbb, and, sub, xor and mov into r/m32
  case 0x01:
  case 0x09:
  case 0x11:
  case 0x19:
  case 0x21:
  case 0x29:
  case 0x31:
  case 0x89:
    return RmIsESP;
  // The same from r/m32 into r32, and lea
  case 0x03:
  case 0x0b:
  case 0x13:
  case 0x1b:
  case 0x23:
  case 0x2b:
  case 0x33:
  case 0x8b:
  case 0x8d:
    return RegIsESP;
  // Arithmetic with an immediate, except cmp (/7), which writes nothing
  case 0x81:
  case 0x83:
    return RmIsESP and Reg != 7;
  // mov r/m32, imm32
  case 0xc7:
    return RmIsESP and Reg == 0;
  default:
    return false;
  }
}

template<typename T, typename... ArgTypes>
constexpr std::array<T, sizeof...(ArgTypes)> make_array(ArgTypes &&...Args) {
  return { { std::forward<ArgTypes>(Args)... } };
}

/// Wrap a value around a temporary opaque function
///
/// Useful to prevent undesired optimizations
class OpaqueIdentity {
private:
  std::map<Type *, Function *> Map;
  Module *M = nullptr;

public:
  OpaqueIdentity(Module *M) : M(M) {}

  ~OpaqueIdentity() { revng_assert(Map.size() == 0); }

  void drop() {
    SmallVector<CallInst *, 16> ToErase;
    for (auto &&[T, F] : Map) {
      for (User *U : F->users()) {
        auto *Call = cast<CallInst>(U);
        Call->replaceAllUsesWith(Call->getArgOperand(0));
        ToErase.push_back(Call);
      }
    }

    for (CallInst *Call : ToErase)
      eraseFromParent(Call);

    for (auto &&[T, F] : Map)
      eraseFromParent(F);

    Map.clear();
  }

  Instruction *wrap(revng::IRBuilder &Builder, Value *V) {
    Type *ResultType = V->getType();
    Function *F = nullptr;
    auto It = Map.find(ResultType);
    if (It == Map.end()) {
      auto *FT = FunctionType::get(ResultType, { ResultType }, false);
      F = Function::Create(FT, GlobalValue::ExternalLinkage, "id", *M);
      F->setOnlyReadsMemory();
      Map[ResultType] = F;
    } else {
      F = It->second;
    }

    return Builder.CreateCall(F, { V });
  }

  Instruction *wrap(Instruction *I) {
    revng::IRBuilder Builder(I->getParent(), ++I->getIterator());
    return wrap(Builder, I);
  }
};

// Outline the destructor for the sake of privacy in the header
CodeGenerator::~CodeGenerator() = default;

CodeGenerator::CodeGenerator(const RawBinaryView &RawBinary,
                             llvm::Module *TheModule,
                             const TupleTree<model::Binary> &Model,
                             std::string Helpers,
                             std::string EarlyLinked,
                             model::Architecture::Values TargetArchitecture) :
  RawBinary(RawBinary),
  TheModule(TheModule),
  Context(TheModule->getContext()),
  Model(Model),
  TargetArchitecture(TargetArchitecture) {

  LibTcgInstrMDKind = Context.getMDKindID("pi");

  HelpersModule = parseIR(Context, Helpers);
  revng_assert(HelpersModule->getGlobalVariable("cpu_loop_exiting") != nullptr);
  TheModule->setDataLayout(HelpersModule->getDataLayout());

  EarlyLinkedModule = parseIR(Context, EarlyLinked);
  // TODO: do this at compile time
  for (llvm::Function &F : *EarlyLinkedModule) {
    if (F.isIntrinsic())
      continue;

    FunctionTags::QEMU.addTo(&F);
  }

  auto *Uint8Ty = Type::getInt8Ty(Context);
  auto *ElfHeaderHelper = new GlobalVariable(*TheModule,
                                             Uint8Ty,
                                             true,
                                             GlobalValue::ExternalLinkage,
                                             ConstantInt::get(Uint8Ty, 0),
                                             "elfheaderhelper");
  ElfHeaderHelper->setAlignment(MaybeAlign(1));
  ElfHeaderHelper->setSection(".elfheaderhelper");
}

void CodeGenerator::translate(LibTcg &LibTcg,
                              std::optional<uint64_t> RawVirtualAddress) {
  using FT = FunctionType;

  Task T(12, "Translation");

  //
  // Link helpers module into the main module
  //
  T.advance("Linking helpers module", true);
  Linker TheLinker(*TheModule);
  bool Result = TheLinker.linkInModule(std::move(HelpersModule));
  revng_assert(not Result, "Linking failed");

  //
  // Create the VariableManager
  //
  bool TargetIsLittleEndian;
  {
    using namespace model::Architecture;
    TargetIsLittleEndian = isLittleEndian(TargetArchitecture);
  }

  const auto &ArchInfo = LibTcg.archInfo();
  VariableManager Variables(*TheModule, TargetIsLittleEndian, LibTcg);
  //
  // Create well-known CSVs
  //
  GlobalVariable *SPReg = Variables.getByEnvOffset(ArchInfo.sp).first;

  using PCHOwner = std::unique_ptr<ProgramCounterHandler>;
  auto Factory = [&Variables,
                  &ArchInfo](PCAffectingCSV::Values CSVID) -> GlobalVariable * {
    intptr_t Offset = 0;

    switch (CSVID) {
    case PCAffectingCSV::PC:
      Offset = ArchInfo.pc;
      break;

    case PCAffectingCSV::IsThumb:
      Offset = ArchInfo.is_thumb;
      break;

    default:
      revng_abort();
    }

    return Variables.getByEnvOffset(Offset).first;
  };

  PCHOwner PCH = ProgramCounterHandler::create(Model->Architecture(),
                                               TheModule,
                                               Factory);

  revng::IRBuilder Builder(Context);

  // Create main function
  auto *MainType = FT::get(Builder.getVoidTy(),
                           { SPReg->getValueType() },
                           false);
  auto *RootFunction = Function::Create(MainType,
                                        Function::ExternalLinkage,
                                        "root",
                                        TheModule);
  FunctionTags::Root.addTo(RootFunction);
  RootFunction->addFnAttr(Attribute::NullPointerIsValid);

  // Create the first basic block and create a placeholder for variable
  // allocations
  BasicBlock *Entry = BasicBlock::Create(Context, "entrypoint", RootFunction);
  Builder.SetInsertPoint(Entry);

  // We need to remember this instruction so we can later insert a call here.
  // The problem is that up until now we don't know where our CPUState structure
  // is.
  // After the translation we will and use this information to create a call to
  // a helper function.
  // TODO: we need a more elegant solution here
  auto *Delimiter = Builder.CreateStore(&*RootFunction->arg_begin(), SPReg);
  Variables.setAllocaInsertPoint(Delimiter);
  auto *InitEnvInsertPoint = Delimiter;

  QuickMetadata QMD(Context);

  // Link early-linked.c
  T.advance("Link early-linked.c", true);
  {
    Linker TheLinker(*TheModule);
    bool Result = TheLinker.linkInModule(std::move(EarlyLinkedModule),
                                         Linker::None);
    revng_assert(!Result, "Linking failed");
  }

  // Create an instance of JumpTargetManager
  JumpTargetManager JumpTargets(RootFunction, PCH.get(), Model, RawBinary);

  MetaAddress VirtualAddress = MetaAddress::invalid();
  if (RawVirtualAddress) {
    VirtualAddress = JumpTargets.fromPC(*RawVirtualAddress);
  } else {
    JumpTargets.harvestGlobalData();
    VirtualAddress = Model->EntryPoint();
  }

  if (VirtualAddress.isValid()) {
    revng_assert(VirtualAddress.isCode());
    JumpTargets.registerJT(VirtualAddress, JumpTargetReason::GlobalData);

    // Initialize the program counter
    PCH->initializePC(Builder, VirtualAddress);
  }

  OpaqueIdentity OI(TheModule);

  // Fake jumps to the dispatcher-related basic blocks. This way all the blocks
  // are always reachable.
  auto *ReachSwitch = Builder.CreateSwitch(OI.wrap(Builder, Builder.getInt8(0)),
                                           JumpTargets.dispatcher());
  ReachSwitch->addCase(Builder.getInt8(1), JumpTargets.anyPC());
  ReachSwitch->addCase(Builder.getInt8(2), JumpTargets.unexpectedPC());

  JumpTargets.setCFGForm(CFGForm::SemanticPreserving);

  std::vector<BasicBlock *> Blocks;

  bool EndiannessMismatch;
  {
    using namespace model::Architecture;
    bool SourceIsLittleEndian = isLittleEndian(Model->Architecture());
    EndiannessMismatch = TargetIsLittleEndian != SourceIsLittleEndian;
  }

  T.advance("Lifting code", true);
  Task LiftTask({}, "Lifting");
  LiftTask.advance("Initial address peeking", false);
  InstructionTranslator Translator(LibTcg,
                                   Builder,
                                   Variables,
                                   JumpTargets,
                                   Blocks,
                                   EndiannessMismatch,
                                   PCH.get());

  std::tie(VirtualAddress, Entry) = JumpTargets.peek();

  while (Entry != nullptr) {
    if (Log.isEnabled()) {
      Log << "Processing " << VirtualAddress.toString() << " due to ";
      const char *Separator = "";
      for (const char *Reason :
           JumpTargets.at(VirtualAddress).getReasonNames()) {
        Log << Separator << Reason;
        Separator = ", ";
      }
      Log << DoLog;
    }

    llvm::ArrayRef<uint8_t> CodeBuffer;
    CodeBuffer = RawBinary.getFromAddressOn(VirtualAddress).value();

    LiftTask.advance(VirtualAddress.toString(), true);

    Task TranslateTask(3, "Translate");
    TranslateTask.advance("Lift to PTC", true);

    Builder.SetInsertPoint(Entry);

    // TODO: what if create a new instance of an InstructionTranslator here?
    Translator.reset();

    uint32_t TranslateFlags = 0;
    if (VirtualAddress.type() == MetaAddressType::Code_arm_thumb) {
      TranslateFlags |= LIBTCG_TRANSLATE_ARM_THUMB;
    }

    auto TranslationBlock = LibTcg.translateBlock(CodeBuffer.data(),
                                                  CodeBuffer.size(),
                                                  VirtualAddress.address(),
                                                  TranslateFlags);

    // TODO: rename this type
    const size_t ConsumedSize = TranslationBlock->size_in_bytes;
    revng_assert(ConsumedSize > 0);

    SmallSet<unsigned, 1> ToIgnore;
    // Handles writes to btarget, represents branching for microblaze/mips/cris
    ToIgnore = Translator.preprocess(*TranslationBlock);

    if (LibTcgLog.isEnabled()) {
      static std::array<char, 128> DumpBuf{ 0 };
      LibTcgLog << "Translation starting from " << VirtualAddress.toGeneric()
                << " (size: " << ConsumedSize << " bytes)" << DoLog;
      LoggerIndent Indent(LibTcgLog);
      LibTcgLog.indent();
      for (size_t I = 0; I < TranslationBlock->instruction_count; ++I) {
        auto Opcode = TranslationBlock->list[I].opcode;
        bool IsInstructionStart = Opcode == LIBTCG_op_insn_start;
        if (IsInstructionStart)
          LibTcgLog.unindent();
        LibTcg.dumpInstructionToBuffer(&TranslationBlock->list[I],
                                       DumpBuf.data(),
                                       DumpBuf.size());
        LibTcgLog << StringRef(DumpBuf.data()).trim();
        if (ToIgnore.contains(I))
          LibTcgLog << " (ignored)";
        LibTcgLog << DoLog;
        if (IsInstructionStart)
          LibTcgLog.indent();
      }
      LibTcgLog.unindent();
    }

    bool StopTranslation = false;

    MetaAddress PC = VirtualAddress;
    MetaAddress NextPC = MetaAddress::invalid();
    MetaAddress EndPC = VirtualAddress + ConsumedSize;

    const auto InstructionCount = TranslationBlock->instruction_count;
    using IT = InstructionTranslator;
    IT::TranslationResult Result;

    unsigned J = 0;

    // Handle the first LIBTCG_op_insn_start
    {
      LibTcgInstruction *NextInstruction = nullptr;
      for (unsigned K = 1; K < InstructionCount; K++) {
        LibTcgInstruction *I = &TranslationBlock->list[K];
        if (I->opcode == LIBTCG_op_insn_start && ToIgnore.count(K) == 0) {
          NextInstruction = I;
          break;
        }
      }
      LibTcgInstruction *Instruction = &TranslationBlock->list[J];
      std::tie(Result, PC, NextPC) = Translator.newInstruction(Instruction,
                                                               NextInstruction,
                                                               VirtualAddress,
                                                               EndPC,
                                                               true);
      J++;
    }

    Variables.openTranslationBlock(*(--Builder.GetInsertPoint()));

    // A path that writes the 32-bit ESP ends there: see
    // writesTruncatedStackPointer
    auto TruncatesStackPointer = [&](MetaAddress Address) {
      if (Address.type() != MetaAddressType::Code_x86_64)
        return false;
      auto Bytes = JumpTargets.binaryView().getByAddress(Address, 15);
      if (not Bytes)
        Bytes = JumpTargets.binaryView().getFromAddressOn(Address);
      return Bytes and writesTruncatedStackPointer(*Bytes);
    };

    if (Result == IT::Success and TruncatesStackPointer(PC)) {
      emitAbort(Builder, "");
      StopTranslation = true;
    }

    unsigned SinceInstructionStart = 0;

    // TODO: shall we move this whole loop in InstructionTranslator?
    for (; J < InstructionCount && !StopTranslation; J++) {
      if (ToIgnore.count(J) != 0)
        continue;

      LibTcgInstruction *Instruction = &TranslationBlock->list[J];
      auto Opcode = Instruction->opcode;

      Blocks.clear();
      if (BasicBlock *Block = Builder.GetInsertBlock()) {
        Blocks.push_back(Block);
      } else {
        revng_assert(Opcode == LIBTCG_op_set_label);
      }

      ++SinceInstructionStart;

      switch (Opcode) {
      case LIBTCG_op_discard:
        // Instructions we don't even consider
        break;
      case LIBTCG_op_insn_start: {
        SinceInstructionStart = 0;

        // Find next instruction, if there is one
        LibTcgInstruction *NextInstruction = nullptr;
        for (unsigned K = J + 1; K < InstructionCount; K++) {
          LibTcgInstruction *I = &TranslationBlock->list[K];
          if (I->opcode == LIBTCG_op_insn_start && ToIgnore.count(K) == 0) {
            NextInstruction = I;
            break;
          }
        }

        std::tie(Result,
                 PC,
                 NextPC) = Translator.newInstruction(Instruction,
                                                     NextInstruction,
                                                     VirtualAddress,
                                                     EndPC,
                                                     false);
        if (Result == IT::Success and TruncatesStackPointer(PC))
          Result = IT::Abort;
      } break;
      case LIBTCG_op_call: {
        Result = Translator.translateCall(Instruction,
                                          PC,
                                          SinceInstructionStart);

        // Sometimes libtinycode terminates a basic block with a call, in this
        // case force a fallthrough, unless the instruction is int3 or `int 3`:
        // the breakpoint exception they raise does not return to the program,
        // and padding of int3 would otherwise be lifted one byte at a time
        auto IsBreakpoint = [&]() {
          if (PC.type() != MetaAddressType::Code_x86
              and PC.type() != MetaAddressType::Code_x86_64)
            return false;
          auto Bytes = JumpTargets.binaryView().getByAddress(PC, 2);
          return Bytes and not Bytes->empty()
                 and ((*Bytes)[0] == 0xcc
                      or (Bytes->size() == 2 and (*Bytes)[0] == 0xcd
                          and (*Bytes)[1] == 0x03));
        };
        if (J == TranslationBlock->instruction_count - 1) {
          BasicBlock *Target = nullptr;
          if (not IsBreakpoint())
            Target = JumpTargets.registerJT(EndPC,
                                            JumpTargetReason::PostHelper);
          if (Target != nullptr) {
            Builder.CreateBr(&notNull(Target));
          } else {
            emitAbort(Builder, "");
          }
        }
      } break;
      default:
        Result = Translator.translate(Instruction,
                                      PC,
                                      SinceInstructionStart,
                                      NextPC);
        break;
      }

      switch (Result) {
      case IT::Success:
        // No-op
        break;
      case IT::Abort:
        emitAbort(Builder, "");
        StopTranslation = true;
        break;
      case IT::Stop:
        StopTranslation = true;
        break;
      }

      // Create a new metadata referencing the TCG instruction we have just
      // translated
      MDNode *MDLibTcgInstr = nullptr;
      if (RecordTCG) {
        static std::array<char, 128> DumpBuf{ 0 };
        LibTcg.dumpInstructionToBuffer(&TranslationBlock->list[J],
                                       DumpBuf.data(),
                                       DumpBuf.size());

        // Eh not very nice to strlen in construction of the StringRef,
        // maybe we can get the length from the LibTcg call above?
        StringRef Str{ DumpBuf.data() };
        MDString *MDLibTcgString = MDString::get(Context, Str);
        MDLibTcgInstr = MDNode::getDistinct(Context, MDLibTcgString);
      }

      // Set metadata for all the new instructions
      for (BasicBlock *Block : Blocks) {
        BasicBlock::iterator I = Block->end();
        while (I != Block->begin() && !(--I)->hasMetadata()) {
          if (MDLibTcgInstr != nullptr)
            I->setMetadata(LibTcgInstrMDKind, MDLibTcgInstr);
        }
      }

    } // End loop over instructions

    TranslateTask.advance("Finalization", true);

    Variables.closeTranslationBlock();

    if (auto *LastBlock = Builder.GetInsertBlock()) {
      if (LastBlock->empty()) {
        eraseFromParent(LastBlock);
      } else if (!LastBlock->rbegin()->isTerminator()) {
        // Something went wrong, probably a mistranslation
        Builder.CreateUnreachable();
      }
    }

    // A conditional jump at the end of the block whose outcome the paths of
    // its function decide: its other successor is junk
    std::optional<MetaAddress> DeadSuccessor;
    if (PruneDecidedBranches and PC.type() == MetaAddressType::Code_x86_64)
      DeadSuccessor = JumpTargets.unwindRanges().deadSuccessor(PC);

    Translator.registerDirectJumps(DeadSuccessor);
    // Obtain a new program counter to translate
    TranslateTask.complete();
    LiftTask.advance("Peek new address", true);
    std::tie(VirtualAddress, Entry) = JumpTargets.peek();
  } // End translations loop

  LiftTask.complete();

  OI.drop();

  // Reorder basic blocks in RPOT
  T.advance("Reordering basic blocks", true);
  {
    BasicBlock *Entry = &RootFunction->getEntryBlock();
    ReversePostOrderTraversal<BasicBlock *> RPOT(Entry);
    std::set<BasicBlock *> SortedBasicBlocksSet;
    std::vector<BasicBlock *> SortedBasicBlocks;
    for (BasicBlock *BB : RPOT) {
      SortedBasicBlocksSet.insert(BB);
      SortedBasicBlocks.push_back(BB);
    }

    std::vector<BasicBlock *> Unreachable;
    for (BasicBlock &BB : *RootFunction)
      if (!SortedBasicBlocksSet.contains(&BB))
        Unreachable.push_back(&BB);

    auto Size = RootFunction->size();
    for (unsigned I = 0; I < Size; ++I)
      RootFunction->begin()->removeFromParent();
    for (BasicBlock *BB : SortedBasicBlocks)
      RootFunction->insert(RootFunction->end(), BB);
    for (BasicBlock *BB : Unreachable)
      RootFunction->insert(RootFunction->end(), BB);
  }

  T.advance("IR finalization", true);

  // Remove the "helpers_list" variable, whose purpose is to keep alive helpers
  // who would otherwise get DCE'd away due to their linkage.
  // At this point we know which ones we want, and we're OK with the dead ones
  // to be dropped.
  TheModule->getGlobalVariable(HelpersListName)->eraseFromParent();

  //
  // Look for calls to functions that might exit and reset cpu_loop_exiting
  //
  auto *CpuLoopExiting = TheModule->getGlobalVariable("cpu_loop_exiting", true);
  auto *BoolType = CpuLoopExiting->getValueType();
  for (BasicBlock &BB : *RootFunction) {
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallInst>(&I);
      if (Call == nullptr)
        continue;

      auto *Callee = getCalledFunction(Call);
      if (Callee == nullptr or not Callee->hasMetadata("revng.cpu_loop_exits"))
        continue;

      new StoreInst(ConstantInt::getFalse(BoolType),
                    CpuLoopExiting,
                    Call->getNextNode());
      // TODO: are we guaranteed to have a check for the PC and go back to the
      //       dispatcher here if there's a mismatch?
    }
  }

  // Add a call to the function to initialize the CPUState, if present.
  // This is important on x86 architecture.
  // We only add the call after the Linker has imported the
  // helper_initialize_env function from the helpers, because the declaration
  // imported before with importHelperFunctionDeclaration() only has
  // stub types and injecting the CallInst earlier would break
  if (std::optional InitEnvHelper = InitializeEnvHelper.get(*TheModule)) {
    Function *InitEnv = InitEnvHelper->function();
    revng_assert(not InitEnv->getFunctionType()->isVarArg());
    revng_assert(InitEnv->getFunctionType()->getNumParams() == 1);
    auto *CPUStateType = InitEnv->getFunctionType()->getParamType(0);
    Instruction *InsertBefore = InitEnvInsertPoint;
    auto *AddressComputation = Variables.computeEnvAddress(CPUStateType,
                                                           InsertBefore);
    CallInst::Create(InitEnv, { AddressComputation }, "", InsertBefore);
  }

  Variables.setDataLayout(&TheModule->getDataLayout());

  T.advance("Finalize newpc markers", true);
  Translator.finalizeNewPCMarkers();

  T.advance("Optimize lifted IR");

  SimpleFunctionPassManager InstCombinePM;
  InstCombinePM.addPass(llvm::SROAPass(SROAOptions::PreserveCFG));
  InstCombinePM.addPass(llvm::InstCombinePass());
  InstCombinePM.addPass(llvm::DCEPass());
  InstCombinePM.run(*RootFunction);

  // Ensure we don't have phis in the dispatcher. This can happen if a tiny code
  // local variable has an uninitialized read, which is usually a bug on our
  // part.
  auto Phis = JumpTargets.dispatcher()->phis();
  revng_assert(Phis.begin() == Phis.end(),
               "A phi has appeared in the dispatcher");

  class RootFunction Root(*TheModule);
  CSVGlobals Globals(*Model, *TheModule);
  legacy::PassManager PostInstCombinePM;
  PostInstCombinePM.add(new FunctionCallIdentification(Root,
                                                       Globals,
                                                       Model->Architecture()));
  PostInstCombinePM.add(new PruneRetSuccessors(Root));
  PostInstCombinePM.add(createGlobalDCEPass());
  PostInstCombinePM.run(*TheModule);

  T.advance("Finalize jump targets", true);
  JumpTargets.finalizeJumpTargets();

  T.advance("Purge dead code", true);
  EliminateUnreachableBlocks(*RootFunction, nullptr, false);

  T.advance("Create revng.jt.reason", true);
  JumpTargets.createJumpTargetReasonMD();

  // Record the ranges the loader makes read-only once it has applied the
  // relocations, for the analyses of the lifted code that need to know which
  // pointers in memory cannot change, such as GOT slots
  {
    QuickMetadata QMD(Context);
    auto *Relro = TheModule->getOrInsertNamedMetadata(RelroMDName);
    for (auto [Start, End] : RawBinary.relro())
      Relro->addOperand(QMD.tuple({ QMD.get(Start), QMD.get(End) }));
  }

  T.advance("Finalization", true);
  ExternalJumpsHandler JumpOutHandler(*Model,
                                      JumpTargets.dispatcher(),
                                      *RootFunction,
                                      PCH.get());
  JumpOutHandler.createExternalJumpsHandler();

  Variables.finalize();
}
