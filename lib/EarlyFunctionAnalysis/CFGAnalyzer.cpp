//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <fstream>
#include <string>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/CodeGen/UnreachableBlockElim.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Progress.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#include "llvm/Transforms/Scalar/MergedLoadStoreMotion.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "revng/ADT/Queue.h"
#include "revng/BasicAnalyses/CSVGlobals.h"
#include "revng/BasicAnalyses/RemoveHelperCalls.h"
#include "revng/BasicAnalyses/RemoveNewPCCalls.h"
#include "revng/EarlyFunctionAnalysis/AAWriterPass.h"
#include "revng/EarlyFunctionAnalysis/BasicBlock.h"
#include "revng/EarlyFunctionAnalysis/CFGAnalyzer.h"
#include "revng/EarlyFunctionAnalysis/CallEdge.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdgeBase.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdgeType.h"
#include "revng/EarlyFunctionAnalysis/IndirectBranchInfo.h"
#include "revng/EarlyFunctionAnalysis/IndirectBranchInfoPrinterPass.h"
#include "revng/EarlyFunctionAnalysis/PreliminaryAnalysisCache.h"
#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/EarlyFunctionAnalysis/SegregateDirectStackAccesses.h"
#include "revng/Model/FunctionAttribute.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/EmitAbort.h"
#include "revng/Support/Generator.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/MetaAddress.h"
#include "revng/Support/MoveToOwnModule.h"
#include "revng/Support/NewPC.h"

// This name is not present after `lift`.

using namespace llvm;

using BranchArgument = efa::IndirectBranchInfoArgument;
constexpr unsigned CallerBlockIDIndex = index(BranchArgument::CallerBlockID);
constexpr unsigned CalledSymbolIndex = index(BranchArgument::CalledSymbol);
constexpr unsigned
  JumpsToReturnAddressIndex = index(BranchArgument::JumpsToReturnAddress);
constexpr unsigned
  StackPointerOffsetIndex = index(BranchArgument::StackPointerOffset);
constexpr unsigned
  ReturnValuePreservedIndex = index(BranchArgument::ReturnValuePreserved);
constexpr unsigned
  FirstPreservedRegisterIndex = index(BranchArgument::FirstPreservedRegister);
constexpr unsigned CallerBlockID = CallerBlockIDIndex;
constexpr unsigned CalledSymbol = CalledSymbolIndex;
constexpr unsigned JumpsToReturnAddress = JumpsToReturnAddressIndex;
constexpr unsigned StackPointerOffset = StackPointerOffsetIndex;
constexpr unsigned ReturnValuePreserved = ReturnValuePreservedIndex;
constexpr unsigned FirstPreservedRegister = FirstPreservedRegisterIndex;
using namespace llvm::cl;

static opt<std::string> AAWriterPath("aa-writer",
                                     desc("Dump to disk the outlined functions "
                                          "with annotated alias info."),
                                     value_desc("filename"));

static opt<std::string> IndirectBranchInfoSummaryPath("indirect-branch-info-"
                                                      "summary",
                                                      desc("Write the results "
                                                           "of SA2 on disk."),
                                                      value_desc("filename"));

static opt<bool> RoundTrip("detect-abi-round-trip",
                           desc("Run the optimization pipeline of each "
                                "preliminary analysis on a copy of the "
                                "function in a context of its own, moved "
                                "there through bitcode, as a worker thread "
                                "would"),
                           init(false));

static Logger Log("cfg-analyzer");

static MetaAddress getFinalAddressOfBasicBlock(llvm::BasicBlock *BB) {
  auto &&[End, Size] = getPC(BB->getTerminator());
  return End + Size;
}

static UpcastablePointer<efa::FunctionEdgeBase>
makeCall(MetaAddress Destination) {
  using ReturnType = UpcastablePointer<efa::FunctionEdgeBase>;
  return ReturnType::make<efa::CallEdge>(BasicBlockID(Destination),
                                         efa::FunctionEdgeType::FunctionCall);
};

static UpcastablePointer<efa::FunctionEdgeBase>
makeEdge(BasicBlockID Destination, efa::FunctionEdgeType::Values Type) {
  revng_assert(Type != efa::FunctionEdgeType::FunctionCall);
  efa::FunctionEdge *Result = nullptr;
  using ReturnType = UpcastablePointer<efa::FunctionEdgeBase>;
  return ReturnType::make<efa::FunctionEdge>(Destination, Type);
};

static UpcastablePointer<efa::FunctionEdgeBase>
makeIndirectEdge(efa::FunctionEdgeType::Values Type) {
  return makeEdge(BasicBlockID::invalid(), Type);
};

namespace efa {

static efa::BasicBlock &
blockFromIndirectBranchInfo(const IndirectBranchFacts &Branch,
                            SortedVector<efa::BasicBlock> &CFG,
                            const DecodedInstructions &Decoded) {
  const BasicBlockID &ID = Branch.Block;
  auto It = CFG.find(ID);
  if (It != CFG.end())
    return *It;

  // The argument is the address of the branch: find the block that decodes
  // an instruction there. Address ranges alone are ambiguous: an overlapping
  // decoding that starts inside an instruction of another block lies within
  // that block's range without being part of it. Among the blocks that
  // decode the branch, the one ending in it gets its only edges from here,
  // so prefer one that has none yet.
  efa::BasicBlock *Result = nullptr;
  for (efa::BasicBlock &Block : CFG) {
    if (not Block.contains(ID) or not Decoded.contains({ Block.ID(), ID }))
      continue;
    if (Block.Successors().empty())
      return Block;
    if (Result == nullptr)
      Result = &Block;
  }

  revng_assert(Result != nullptr, "No block decodes the indirect branch");
  return *Result;
}

static std::unique_ptr<llvm::raw_ostream>
streamFromOption(const opt<std::string> &Option) {
  if (Option.getNumOccurrences() == 1) {
    std::ifstream File(Option.c_str());
    if (File.is_open()) {
      int Status = std::remove(Option.c_str());
      revng_assert(Status == 0);
    }

    std::error_code EC;
    auto Result = std::make_unique<raw_fd_ostream>(Option,
                                                   EC,
                                                   llvm::sys::fs::OF_Append);
    revng_assert(!EC);
    return Result;
  } else {
    return std::make_unique<llvm::raw_null_ostream>();
  }
}

// Some LLVM passes used in the pipeline scan for cut-offs, meaning that further
// computation may not be done when they are reached; making some optimizations
// opportunities missed. Hence, we set the involved thresholds (e.g., the
// maximum value that MemorySSA uses to take into account stores/phis) to have
// initial unbounded value.
UnboundedScanLimits::UnboundedScanLimits() :
  MemorySSALimit("memssa-check-limit", UINT_MAX),
  MemoryDependenceLimit("memdep-block-scan-limit", UINT_MAX) {
  ++Active;
}

CFGAnalyzer::CFGAnalyzer(llvm::Module &M,
                         RootFunction &Root,
                         const CSVGlobals &Globals,
                         const TupleTree<model::Binary> &Binary,
                         FunctionSummaryOracle &Oracle) :
  M(M),
  Globals(Globals),
  PCH(ProgramCounterHandler::fromModule(Binary->Architecture(), &M)),
  Oracle(Oracle),
  Binary(Binary),
  PreCallHook(createCallMarkerType(M), "precall_hook", &M),
  PostCallHook(PreCallHook.get()->getFunctionType(), "postcall_hook", &M),
  RetHook(createRetMarkerType(M), "retcall_hook", &M),
  Outliner(M, Root, Globals, Oracle),
  OpaqueBranchConditionsPool(&M, false),
  OutputAAWriter(streamFromOption(AAWriterPath)),
  OutputIBI(streamFromOption(IndirectBranchInfoSummaryPath)) {

  // Collect all ABI CSVs except for the stack pointer
  for (GlobalVariable *CSV : Globals.abiRegisters())
    if (CSV != nullptr and not Globals.isSPReg(CSV))
      ABICSVs.emplace_back(CSV);

  // Prepare header for debugging information about indirect branch infos
  *OutputIBI << "name,ra,fso,address";
  for (const auto &Register : ABICSVs)
    *OutputIBI << "," << Register->getName();
  *OutputIBI << "\n";
}

OutlinedFunction CFGAnalyzer::outline(const MetaAddress &Entry) {
  auto &CFG = Oracle.getLocalFunction(Entry).CFG;
  if (Recording != nullptr)
    Recording->recordOwnCFG(CFG);
  bool HasCFG = CFG.size() != 0;
  llvm::SmallSet<MetaAddress, 4> ReturnBlocks;

  if (HasCFG)
    for (const efa::BasicBlock &Block : CFG)
      for (const auto &Successor : Block.Successors())
        if (Successor->Type() == efa::FunctionEdgeType::Return)
          ReturnBlocks.insert(Block.ID().start());

  CallSummarizer Summarizer(&M,
                            PreCallHook.get(),
                            PostCallHook.get(),
                            RetHook.get(),
                            Globals.spReg(),
                            HasCFG ? &ReturnBlocks : nullptr);

  OutlinedFunction Result = Outliner.outline(Entry, &Summarizer);

  // Only the new function needs its blocks split. Walking every caller of the
  // hooks and of `newpc` in the module instead would walk the whole of `root`
  // and every function outlined before, for each function.
  auto IsFirst = [](llvm::Instruction *I) {
    return I->getParent()->getFirstNonPHI() == I;
  };
  llvm::SmallVector<llvm::Instruction *, 16> ToSplit;
  auto SplitAll = [&ToSplit]() {
    for (llvm::Instruction *I : ToSplit)
      I->getParent()->splitBasicBlock(I);
    ToSplit.clear();
  };

  // Make sure we start a new block before a PreCallHook
  for (llvm::Instruction &I : llvm::instructions(Result.Function.get()))
    if (llvm::CallInst *Call = getCallTo(&I, PreCallHook.get()))
      if (not IsFirst(Call))
        ToSplit.push_back(Call);
  SplitAll();

  // Make sure we start a new block for each jump target
  revng_assert(NewPCHelper.get(M).has_value());
  for (llvm::Instruction &I : llvm::instructions(Result.Function.get()))
    if (std::optional Call = NewPCHelper.getCall(&I))
      if (startsBasicBlock(*Call) and not IsFirst(Call->call()))
        ToSplit.push_back(Call->call());
  SplitAll();

  return Result;
}

llvm::FunctionType *CFGAnalyzer::createCallMarkerType(llvm::Module &M) {
  auto &Context = M.getContext();
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  Type *BoolType = Type::getInt1Ty(Context);
  Type *Void = Type::getVoidTy(Context);
  return llvm::FunctionType::get(Void,
                                 { I8Ptr, I8Ptr, I8Ptr, BoolType },
                                 false);
}

llvm::FunctionType *CFGAnalyzer::createRetMarkerType(llvm::Module &M) {
  auto &Context = M.getContext();
  Type *Void = Type::getVoidTy(Context);
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  return llvm::FunctionType::get(Void, { I8Ptr }, false);
}

std::optional<UpcastablePointer<efa::FunctionEdgeBase>>
CFGAnalyzer::handleCall(llvm::CallInst *PreCallHookCall) {
  using namespace llvm;

  //
  // Extract parameters
  //

  // Is this a direct call to a function defined in the model?
  Value *CalleePC = PreCallHookCall->getArgOperand(1);
  MetaAddress CalleeAddress = MetaAddress::invalid();
  bool IsDirectCall = false;
  auto Address = BasicBlockID::fromValue(CalleePC).notInlinedAddress();
  if (Address.isValid()) {
    IsDirectCall = Binary->Functions().count(Address) != 0;
    if (Recording != nullptr)
      Recording->recordIsFunction(Address, IsDirectCall);
    if (IsDirectCall)
      CalleeAddress = Address;
  }

  // Symbol name
  StringRef SymbolName;
  auto *SymbolArgument = PreCallHookCall->getArgOperand(2);
  bool IsDynamicCall = not isa<ConstantPointerNull>(SymbolArgument);
  if (IsDynamicCall)
    SymbolName = extractFromConstantStringPtr(SymbolArgument);

  // Tail call?
  bool IsTailCall = (getLimitedValue(PreCallHookCall->getArgOperand(3)) == 1);

  // Here we handle only direct, non-tail calls
  if (not IsDirectCall and IsTailCall)
    return std::nullopt;

  if (IsDynamicCall)
    revng_assert(not IsDirectCall);

  //
  // Construct call edge
  //
  UpcastablePointer<efa::FunctionEdgeBase> Edge = makeCall(CalleeAddress);

  auto *CE = cast<efa::CallEdge>(Edge.get());
  CE->IsTailCall() = IsTailCall;
  if (IsDynamicCall)
    CE->DynamicFunction() = SymbolName.str();

  return Edge;
}

SortedVector<efa::BasicBlock>
CFGAnalyzer::collectDirectCFG(OutlinedFunction *OF,
                              DecodedInstructions &Decoded) {
  using namespace llvm;
  using llvm::BasicBlock;
  revng_log(Log, "collectDirectCFG(" << OF->Function->getName().str() << ")");
  LoggerIndent Indent(Log);

  SortedVector<efa::BasicBlock> CFG;

  for (BasicBlock &BB : *OF->Function) {
    if (isJumpTarget(&BB)) {
      // Create a efa::BasicBlock for each jump target
      BasicBlockID ID = getBasicBlockID(&BB);
      MetaAddress End = getFinalAddressOfBasicBlock(&BB);
      revng_assert(End.isValid());

      efa::BasicBlock Block{ ID };
      Block.End() = End;
      Block.InlinedFrom() = OF->InlinedFunctionsByIndex.at(ID.inliningIndex());
      bool ReachesUnexpectedPC = false;

      // Initialize the end address of the basic block, we'll extend it later on
      revng_log(Log,
                "Creating block starting at " << ID.toString()
                                              << " (preliminary ending is "
                                              << Block.End().toString() << ")");
      LoggerIndent Indent(Log);

      OnceQueue<BasicBlock *> Queue;
      Queue.insert(&BB);

      bool ReachesUnreachable = false;

      while (!Queue.empty()) {
        BasicBlock *Current = Queue.pop();
        revng_log(Log, "Analyzing block " << getName(Current));

        // Record the instructions this block decodes
        for (Instruction &I : *Current)
          if (std::optional NewPCCall = NewPCHelper.getCall(&I))
            Decoded.insert({ ID, blockIDFromNewPC(*NewPCCall) });

        // If this block belongs to a single `newpc`, record its address
        MetaAddress CurrentBlockEnd = getFinalAddressOfBasicBlock(Current);
        if (CurrentBlockEnd.isValid() and CurrentBlockEnd > Block.End()) {
          revng_log(Log,
                    "Extending block end to " << CurrentBlockEnd.toString());
          Block.End() = CurrentBlockEnd;
        }

        if (isa<UnreachableInst>(Current->getTerminator())) {
          revng_log(Log, "Reaches unreachable");
          ReachesUnreachable = true;
        } else {
          revng_assert(succ_size(Current) > 0);
        }

        revng_log(Log, "Considering successors");
        LoggerIndent Indent2(Log);

        for (BasicBlock *Succ : successors(Current)) {
          revng_log(Log, "Considering successor " << getName(Succ));
          if (isa<ReturnInst>(Succ->getTerminator())) {
            revng_log(Log, "It's a ret");
            // Did we meet the end of the cloned function? Do nothing
            revng_assert(Succ->size() == 1);
          } else if (auto *Call = getCallTo(&*Succ->begin(),
                                            PreCallHook.get())) {
            // Handle edge for regular function calls
            if (auto MaybeEdge = handleCall(Call); MaybeEdge) {
              revng_log(Log, "It's a direct call, emitting a CallEdge");
              Block.Successors().insert(*MaybeEdge);
            }
          } else if (isJumpTarget(Succ)) {
            // TODO: handle situation in which it's a *direct* tail call.
            //       We might need an IBI here to know if the stack position is
            //       compatible with a tail call.
            BasicBlockID Destination = getBasicBlockID(Succ);
            revng_log(Log,
                      "It's a jump target: emitting a DirectBranch to "
                        << Destination.toString());
            auto Edge = makeEdge(Destination,
                                 efa::FunctionEdgeType::DirectBranch);
            Block.Successors().insert(Edge);
          } else if (Succ == OF->UnexpectedPCCloned) {
            revng_log(Log, "Reaches UnexpectedPC");
            ReachesUnexpectedPC = true;
          } else {

            revng_log(Log, "Nothing special, enqueue");
            // Not one of the cases above? Enqueue the successor basic block.
            Queue.insert(Succ);
          }
        }
      }

      bool HasNoSuccessor = Block.Successors().size() == 0;
      if (HasNoSuccessor) {
        if (ReachesUnreachable) {
          // If we reach any unreachable instruction, add a single unreachable
          // edge
          revng_log(Log, "Reaches unreachable, add to successors");
          revng_assert(Block.Successors().empty());
          using namespace efa::FunctionEdgeType;
          auto NewEdge = makeEdge(BasicBlockID::invalid(), Unreachable);
          Block.Successors().insert(NewEdge);
        } else if (ReachesUnexpectedPC) {
          // successor of the current basic block.
          revng_log(Log,
                    "No other successors other than UnexpectedPC, emitting "
                    "Unexpected");
          auto Edge = makeEdge(BasicBlockID::invalid(),
                               efa::FunctionEdgeType::Unexpected);
          Block.Successors().insert(Edge);
        }
      }

      // Commit the newly created block to the CFG
      CFG.insert(Block);
    }
  }

  return CFG;
}

CFGAnalyzer::State CFGAnalyzer::loadState(revng::IRBuilder &Builder) const {
  using namespace llvm;
  LLVMContext &Context = M.getContext();

  // Load the stack pointer
  auto *SP0 = Builder.createLoad(Globals.spReg());

  // Load the return address
  Value *ReturnAddress = nullptr;
  if (GlobalVariable *Register = Globals.raReg()) {
    ReturnAddress = Builder.createLoad(Register);
  } else {
    auto *OpaquePointer = PointerType::get(Context, 0);
    auto *StackPointerPointer = Builder.CreateIntToPtr(SP0, OpaquePointer);
    ReturnAddress = Builder.CreateLoad(Globals.pcReg()->getValueType(),
                                       StackPointerPointer);
  }

  // Load the PC
  auto DissectedPC = PCH->dissectJumpablePC(Builder,
                                            ReturnAddress,
                                            Binary->Architecture());
  Value *IntegerPC = MetaAddress::composeIntegerPC(Builder,
                                                   DissectedPC[0],
                                                   DissectedPC[1],
                                                   DissectedPC[2],
                                                   DissectedPC[3]);

  // Load all CSVs
  SmallVector<Value *, 16> CSVs;
  Type *IsRetTy = Type::getInt128Ty(Context);
  for (auto *CSR : ABICSVs) {
    auto *V = Builder.createLoad(CSR);
    CSVs.emplace_back(V);
  }

  return { SP0, IntegerPC, CSVs };
}

static cppcoro::generator<llvm::Instruction *>
previousInstructions(llvm::BasicBlock *BB) {
  do {
    for (Instruction &I : make_range(BB->rbegin(), BB->rend()))
      co_yield &I;
  } while ((BB = BB->getSinglePredecessor()));
}

void CFGAnalyzer::createIBIMarker(OutlinedFunction *Outlined) {
  using namespace llvm;
  using llvm::BasicBlock;

  revng::IRBuilder Builder(&Outlined->Function->getEntryBlock().front());

  State Initial = loadState(Builder);

  //
  // Create IBI for this function
  //
  LLVMContext &Context = M.getContext();
  auto *IntTy = Globals.spReg()->getValueType();
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  SmallVector<Type *, 16> ArgTypes;
  ArgTypes.resize(FirstPreservedRegisterIndex);
  ArgTypes[CallerBlockIDIndex] = I8Ptr;
  ArgTypes[CalledSymbolIndex] = I8Ptr;
  ArgTypes[JumpsToReturnAddressIndex] = Initial.ReturnPC->getType();
  ArgTypes[StackPointerOffsetIndex] = Initial.StackPointer->getType();
  ArgTypes[ReturnValuePreservedIndex] = Initial.ReturnPC->getType();
  for (auto *CSV : ABICSVs)
    ArgTypes.emplace_back(CSV->getValueType());

  auto *FTy = llvm::FunctionType::get(IntTy, ArgTypes, false);
  auto *IBI = IndirectBranchInfo.create(M, FTy, GlobalValue::ExternalLinkage)
                .function();
  Outlined->IndirectBranchInfoMarker = UniqueValuePtr<Function>(IBI);
  Outlined->IndirectBranchInfoMarker->addFnAttr(Attribute::NoUnwind);
  Outlined->IndirectBranchInfoMarker->addFnAttr(Attribute::NoReturn);

  // When an indirect jump is encountered we load the state at that point and
  // compare it against the initial state

  // Initialize markers for ABI analyses and set up the branches on which
  // `indirect_branch_info` will be installed.
  SmallVector<Instruction *, 16> BranchesToAnyPC;
  if (Outlined->AnyPCCloned == nullptr)
    return;

  SmallVector<BasicBlock *, 16> Predecessors;
  for (BasicBlock *Predecessor : predecessors(Outlined->AnyPCCloned))
    Predecessors.push_back(Predecessor);

  for (BasicBlock *Predecessor : Predecessors) {
    Instruction *Term = Predecessor->getTerminator();

    // Create a block and jump there
    auto *IBIBlock = BasicBlock::Create(Context,
                                        Predecessor->getName()
                                          + Twine("_indirect_branch_info"),
                                        Outlined->Function.get(),
                                        nullptr);

    Term->replaceUsesOfWith(Outlined->AnyPCCloned, IBIBlock);

    Builder.SetInsertPoint(IBIBlock);

    // Load the state
    State Final = loadState(Builder);

    // Prepare the arguments for the indirect_branch_info probe call
    SmallVector<Value *, 16> ArgValues;
    ArgValues.resize(FirstPreservedRegisterIndex);

    // Record the MetaAddress of the caller. Lifting can merge consecutive jump
    // targets into one block, so prefer the address of the branch instruction
    // itself: the branch belongs to the CFG block containing that address,
    // which is not necessarily the jump target the merged block starts with.
    auto NewPCID = getBasicBlockID(getJumpTargetBlock(Term->getParent()));
    revng_assert(NewPCID.isValid());
    auto &&[BranchPC, BranchSize] = getPC(Term);
    if (BranchPC.isValid() and NewPCID.start() <= BranchPC)
      NewPCID = BasicBlockID(BranchPC, NewPCID.inliningIndex());
    ArgValues[CallerBlockIDIndex] = NewPCID.toValue(getModule(Term));

    // Record the name of the symbol, if any
    using CPN = ConstantPointerNull;
    Value *SymbolName = CPN::get(Type::getInt8PtrTy(Context));
    BasicBlock *BB = Term->getParent();

    for (Instruction *I : previousInstructions(BB)) {
      if (auto *Call = getCallTo(I, PreCallHook.get())) {
        SymbolName = Call->getArgOperand(2);
        break;
      }
    }

    ArgValues[CalledSymbolIndex] = SymbolName;

    // Compute the difference between the final PC at this program point and
    // the expected return address
    auto *FinalPC = PCH->composeIntegerPC(Builder);
    ArgValues[JumpsToReturnAddressIndex] = Builder.CreateSub(FinalPC,
                                                             Initial.ReturnPC);

    // Compute the difference between the initial stack pointer and the stack
    // pointer at this program point
    Value *Difference = Builder.CreateSub(Final.StackPointer,
                                          Initial.StackPointer);
    ArgValues[StackPointerOffsetIndex] = Difference;

    // Check if the expected ReturnPC has been preserved
    ArgValues[ReturnValuePreservedIndex] = Builder.CreateSub(Final.ReturnPC,
                                                             Initial.ReturnPC);

    // Compute the difference between the initial and final values of the CSVs
    for (const auto &[Initial, End] : zip(Initial.CSVs, Final.CSVs)) {
      auto *ABIRegistersDifference = Builder.CreateSub(Initial, End);
      ArgValues.emplace_back(ABIRegistersDifference);
    }

    // Emit the `indirect_branch_info` call
    Builder.CreateCall(Outlined->IndirectBranchInfoMarker.get(), ArgValues);
    Builder.CreateUnreachable();
  }
}

void CFGAnalyzer::opaqueBranchConditions(llvm::Function *F,
                                         revng::IRBuilder &IRB) {
  using namespace llvm;

  for (auto &BB : *F) {
    auto *Term = BB.getTerminator();
    if ((isa<BranchInst>(Term) && cast<BranchInst>(Term)->isConditional())
        || isa<SwitchInst>(Term)) {
      Value *Condition = isa<BranchInst>(Term) ?
                           cast<BranchInst>(Term)->getCondition() :
                           cast<SwitchInst>(Term)->getCondition();

      OpaqueBranchConditionsPool.addFnAttribute(Attribute::NoUnwind);
      auto MemoryEffects = MemoryEffects::inaccessibleMemOnly();
      OpaqueBranchConditionsPool.setMemoryEffects(MemoryEffects);
      OpaqueBranchConditionsPool.addFnAttribute(Attribute::WillReturn);
      OpaqueBranchConditionsPool.setTags({ &FunctionTags::UniquedByPrototype });

      auto *FTy = llvm::FunctionType::get(Condition->getType(),
                                          { Condition->getType() },
                                          false);

      auto *OpaqueTrueCallee = OpaqueBranchConditionsPool.get(FTy,
                                                              FTy,
                                                              "opaque_true");

      IRB.SetInsertPoint(Term);
      auto *RetVal = IRB.CreateCall(OpaqueTrueCallee, { Condition });

      if (isa<BranchInst>(Term))
        cast<BranchInst>(Term)->setCondition(RetVal);
      else
        cast<SwitchInst>(Term)->setCondition(RetVal);
    }
  }
}

void CFGAnalyzer::materializePCValues(llvm::Function *F,
                                      revng::IRBuilder &Builder) {
  for (llvm::BasicBlock &BB : *F) {
    for (llvm::Instruction &I : BB) {
      if (std::optional Call = NewPCHelper.getCall(&I)) {
        MetaAddress NewPC = blockIDFromNewPC(*Call).start();
        Builder.SetInsertPoint(Call->call());
        PCH->setPC(Builder, NewPC);
      }
    }
  }
}

void CFGAnalyzer::runOptimizationPipeline(llvm::Function &F,
                                          llvm::GlobalVariable *StackPointer,
                                          llvm::raw_ostream &OutputIBI,
                                          llvm::raw_ostream &OutputAAWriter) {
  using namespace llvm;

  // TODO: break it down in the future, and check if some passes can be dropped
  {
    FunctionPassManager FPM;

    // First stage: simplify the IR, promote the CSVs to local variables,
    // compute subexpressions elimination and resolve redundant expressions in
    // order to compute the stack height.
    FPM.addPass(RemoveNewPCCallsPass());
    FPM.addPass(RemoveHelperCallsPass(StackPointer));
    FPM.addPass(PromoteGlobalToLocalPass());
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(JumpThreadingPass());

    // No other pass after JumpThreading depends or uses LVI.
    // LazyValueAnalysis registers deletion callbacks that get called by
    // following modifications of the IR. The cost of calling these callbacks
    // can get very high, and nobody will use the results anyway.
    // So let's invalidate LazyValueAnalysis straight after JT.
    FPM.addPass(InvalidateAnalysisPass<LazyValueAnalysis>());

    FPM.addPass(UnreachableBlockElimPass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(MergedLoadStoreMotionPass());
    FPM.addPass(GVNPass());

    // Second stage: add alias analysis info and canonicalize `i2p` + `add` into
    // `getelementptr` instructions. Since the IR may change remarkably, another
    // round of passes is necessary to take more optimization opportunities.
    FPM.addPass(SegregateDirectStackAccessesPass(StackPointer));
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(InstCombinePass());
    FPM.addPass(GVNPass());

    // Third stage: if enabled, serialize the results and dump the functions on
    // disk with the alias information included as comments.
    if (IndirectBranchInfoSummaryPath.getNumOccurrences() == 1)
      FPM.addPass(IndirectBranchInfoPrinterPass(OutputIBI));

    if (AAWriterPath.getNumOccurrences() == 1)
      FPM.addPass(AAWriterPass(OutputAAWriter));

    ModuleAnalysisManager MAM;

    FunctionAnalysisManager FAM;
    FAM.registerPass([] {
      AAManager AA;
      AA.registerFunctionAnalysis<BasicAA>();
      AA.registerFunctionAnalysis<ScopedNoAliasAA>();

      return AA;
    });
    FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(MAM); });

    PassBuilder PB;
    PB.registerFunctionAnalyses(FAM);
    PB.registerModuleAnalyses(MAM);

    FPM.run(F, FAM);
  }
}

class ClobberedRegistersRegistry {
public:
  using CSVVector = llvm::SmallVector<GlobalVariable *, 16>;

private:
  const CSVVector &ABICSVs;
  CSVSet ClobberedRegs;

public:
  ClobberedRegistersRegistry(const CSVVector &ABICSVs) : ABICSVs(ABICSVs) {}

public:
  const CSVSet &getClobberedRegisters() const { return ClobberedRegs; }

public:
  void recordClobberedRegisters(const IndirectBranchFacts &Branch) {
    revng_assert(Branch.PreservedRegisters.size() == ABICSVs.size());
    for (auto &&[CSV, Preserved] : zip(ABICSVs, Branch.PreservedRegisters))
      if (not Preserved)
        ClobberedRegs.insert(CSV);
  }

  void add(const CSVSet &Clobbered) {
    for (auto *GV : Clobbered)
      ClobberedRegs.insert(GV);
  }
};

/// Elect a final stack offset to tell whether the function is leaving
/// the stack pointer higher than it was at the function entry.
static std::optional<int64_t> electFSO(const auto &MaybeReturns) {
  auto It = std::min_element(MaybeReturns.begin(),
                             MaybeReturns.end(),
                             [](const auto &LHS, const auto &RHS) {
                               return std::get<1>(LHS) < std::get<1>(RHS);
                             });
  if (It == MaybeReturns.end())
    return {};
  return std::get<1>(*It);
}

static bool isNoReturn(const model::Binary &Binary,
                       FunctionSummaryOracle &Oracle,
                       const efa::CallEdge &Edge,
                       PreliminaryAnalysisInputs *Recording) {
  using namespace model::FunctionAttribute;
  if (Edge.Attributes().contains(NoReturn))
    return true;

  if (not Edge.DynamicFunction().empty()) {
    bool Result = Binary.ImportedDynamicFunctions()
                    .at(Edge.DynamicFunction())
                    .Attributes()
                    .contains(NoReturn);
    if (Recording != nullptr)
      Recording->recordDynamicNoReturn(Edge.DynamicFunction(), Result);
    return Result;
  }

  if (Edge.Destination().isValid()) {
    MetaAddress Callee = Edge.Destination().notInlinedAddress();
    bool Result = Oracle.getLocalFunction(Callee).Attributes.contains(NoReturn);
    if (Recording != nullptr)
      Recording->recordLocalNoReturn(Callee, Result);
    return Result;
  }

  return false;
}

std::vector<IndirectBranchFacts>
CFGAnalyzer::readIndirectBranchInfo(llvm::Function &F,
                                    llvm::Function &Marker) {
  using namespace llvm;
  std::vector<IndirectBranchFacts> Result;

  for (CallBase *CI : callers(&Marker)) {
    if (CI->getParent()->getParent() != &F)
      continue;

    IndirectBranchFacts &Branch = Result.emplace_back();
    Branch.Block = BasicBlockID::fromValue(CI->getArgOperand(CallerBlockID));

    auto *CalledSymbolArgument = CI->getArgOperand(CalledSymbolIndex);
    Branch.CalledSymbol = extractFromConstantStringPtr(CalledSymbolArgument);

    // Is this indirect branch targeting jumping to the return address?
    Value *Argument = CI->getArgOperand(JumpsToReturnAddressIndex);
    if (auto *ConstantOffset = dyn_cast<ConstantInt>(Argument))
      Branch.JumpsToReturnAddress = ConstantOffset->getSExtValue() == 0;

    Argument = CI->getArgOperand(StackPointerOffsetIndex);
    if (auto *StackPointerOffset = dyn_cast<ConstantInt>(Argument))
      Branch.StackPointerOffset = StackPointerOffset->getSExtValue();

    Argument = CI->getArgOperand(ReturnValuePreservedIndex);
    auto *ReturnValuePreserved = dyn_cast<ConstantInt>(Argument);
    Branch.ReturnAddressPreserved = ReturnValuePreserved != nullptr
                                    and ReturnValuePreserved->isZero();

    for (unsigned I = FirstPreservedRegisterIndex; I < CI->arg_size(); ++I) {
      auto *Register = dyn_cast<ConstantInt>(CI->getArgOperand(I));
      Branch.PreservedRegisters.push_back(Register != nullptr
                                          and Register->isZero());
    }
  }

  return Result;
}

FunctionSummary
CFGAnalyzer::milkInfo(const MetaAddress &Address,
                      SortedVector<efa::BasicBlock> &&CFG,
                      const DecodedInstructions &Decoded,
                      const std::vector<IndirectBranchFacts> &Branches) {
  using namespace llvm;
  using namespace efa::FunctionEdgeType;
  using namespace model::Architecture;
  int64_t CallPushSize = getCallPushSize(Binary->Architecture());

  revng_log(Log, "Milking info for " << Address.toString());
  LoggerIndent Ident(Log);

  revng_log(Log, "Initial CFG:\n");
  if (Log.isEnabled()) {
    LoggerIndent Ident(Log);
    for (const efa::BasicBlock &Block : CFG) {
      serialize(Log, Block);
      Log << DoLog;
    }
  }

  using BranchFacts = const IndirectBranchFacts *;
  using EdgeType = UpcastablePointer<efa::FunctionEdgeBase>;
  SmallVector<std::pair<BranchFacts, EdgeType>, 4> IBIResult;

  // Temporary bins for IBIs that need extra processing
  SmallVector<std::tuple<BranchFacts, int64_t, const FunctionSummary *>, 4>
    TailCalls;
  SmallVector<std::pair<BranchFacts, int64_t>, 4> MaybeReturns;
  SmallVector<std::tuple<BranchFacts, int64_t, const FunctionSummary *>, 4>
    MaybeIndirectTailCalls;

  for (const IndirectBranchFacts &Branch : Branches) {
    BranchFacts CI = &Branch;
    bool JumpsToReturnAddress = Branch.JumpsToReturnAddress;

    const efa::BasicBlock &Block = blockFromIndirectBranchInfo(Branch,
                                                               CFG,
                                                               Decoded);

    // Is this a tail call? If so, we are very interested in the FSO since
    // it's useful to determine the FSO of the caller
    auto &&[Summary, IsTailCall] = Oracle.getCallSite(Address,
                                                      Block.ID(),
                                                      MetaAddress::invalid(),
                                                      Branch.CalledSymbol);

    if (Branch.StackPointerOffset.has_value()) {
      int64_t FSO = *Branch.StackPointerOffset;
      if (JumpsToReturnAddress) {
        if (FSO >= CallPushSize) {
          // We're jumping to the return address with a non-negative stack
          // offset. This might be a return instruction. Note it down as a
          // candidate
          MaybeReturns.emplace_back(CI, FSO);
        } else {
          // We're jumping to the return address but we're leaving the stack
          // higher than it was initially, it might be the return instruction
          // of a function to inline. Record it as BrokenReturn.
          IBIResult.emplace_back(CI, makeIndirectEdge(BrokenReturn));
        }
      } else if (IsTailCall) {
        if (Summary->ElectedFSO.has_value()) {
          // We have a tail call for which the FSO is known! We can exploit this
          // to know this function's FSO!
          TailCalls.emplace_back(CI, FSO + *Summary->ElectedFSO, Summary);
        }
      } else {
        if (Branch.ReturnAddressPreserved
            and Summary->ElectedFSO.has_value()) {
          // Here the stack pointer offset is known and the value of the
          // return address (in the link register/top of the stack) has been
          // preserved.
          // This is definitely not a return but it might be an indirect
          // tail call.
          MaybeIndirectTailCalls.emplace_back(CI,
                                              FSO + *Summary->ElectedFSO,
                                              Summary);
        } else {
          // The return address is not preserved, something fishy is going on
          IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
        }
      }
    } else {
      // We're leaving the stack pointer in an unknown state
      IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
    }
  }

  if (Log.isEnabled()) {
    Log << "TailCalls:\n";
    for (const auto &[Branch, FSO, Summary] : TailCalls) {
      Log << "  " << Branch->Block.toString() << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;

    Log << "MaybeReturns:\n";
    for (const auto &[Branch, FSO] : MaybeReturns) {
      Log << "  " << Branch->Block.toString() << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;

    Log << "MaybeIndirectTailCalls:\n";
    for (const auto &[Branch, FSO, Summary] : MaybeIndirectTailCalls) {
      Log << "  " << Branch->Block.toString() << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;
  }

  // Elect a final stack offset
  std::optional<int64_t> MaybeWinFSO;

  // Elect a set of clobbered registers on all return points (i.e., actual
  // returns + actual tail calls)

  ClobberedRegistersRegistry ClobberedRegisters(ABICSVs);

  //
  // Process TailCalls
  //

  // Check all tail calls have voted the same FSO, emit a warning otherwise
  if (TailCalls.size() > 0) {
    MaybeWinFSO = electFSO(TailCalls);
    bool Different = false;
    for (auto &&[CI, FSO, Summary] : TailCalls) {
      if (FSO == *MaybeWinFSO) {
        ClobberedRegisters.recordClobberedRegisters(*CI);
        ClobberedRegisters.add(Summary->ClobberedRegisters);
      } else {
        // We found a tail call that leads to a FSO that's not coherent with
        // the elected one. Purge it turning it into a LongJmp.
        Different = true;
        efa::BasicBlock &Block = blockFromIndirectBranchInfo(*CI,
                                                             CFG,
                                                             Decoded);
        revng_assert(Block.Successors().size() == 1);
        auto OldEdge = cast<efa::CallEdge>(Block.Successors().begin()->get());
        revng_assert(OldEdge->IsTailCall());
        Block.Successors() = { makeIndirectEdge(LongJmp) };
      }
    }

    if (Different) {
      revng_log(Log,
                "We have multiple tail calls which lead to uncoherent final "
                "stack sizes");
    }
  }

  //
  // Process MaybeReturns
  //

  // Use candidate returns to elect FSO, if necessary
  if (MaybeReturns.size() > 0) {
    if (not MaybeWinFSO.has_value())
      MaybeWinFSO = electFSO(MaybeReturns);
    revng_assert(MaybeWinFSO.has_value());
  }

  // Divide entries in MaybeReturns in Returns and BrokenReturns
  for (const auto &[CI, FSO] : MaybeReturns) {
    if (MaybeWinFSO.has_value() and FSO == *MaybeWinFSO) {
      // It's a return!
      IBIResult.emplace_back(CI, makeIndirectEdge(Return));
      ClobberedRegisters.recordClobberedRegisters(*CI);
    } else {
      // It's jumping to the return address but leaves the stack pointer in a
      // state not compatible with being a return instruction, it might be the
      // return instruction of a function to inline. Record it as BrokenReturn.
      IBIResult.emplace_back(CI, makeIndirectEdge(BrokenReturn));
    }
  }

  //
  // Process MaybeIndirectTailCalls
  //

  // If no FSO was elected, retry focusing  on MaybeIndirectTailCalls
  if (not MaybeWinFSO.has_value()) {
    revng_assert(MaybeReturns.size() == 0);
    MaybeWinFSO = electFSO(MaybeIndirectTailCalls);
  }

  for (const auto &[CI, FSO, Summary] : MaybeIndirectTailCalls) {
    if (MaybeWinFSO.has_value() && FSO == *MaybeWinFSO) {
      auto NewEdge = makeCall(MetaAddress::invalid());
      auto *Call = cast<efa::CallEdge>(NewEdge.get());
      Call->IsTailCall() = true;
      Call->DynamicFunction() = CI->CalledSymbol;
      IBIResult.emplace_back(CI, std::move(NewEdge));
      ClobberedRegisters.recordClobberedRegisters(*CI);
      ClobberedRegisters.add(Summary->ClobberedRegisters);
    } else {
      IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
    }
  }

  if (Log.isEnabled()) {
    Log << "MaybeWinFSO: ";
    if (MaybeWinFSO)
      Log << *MaybeWinFSO;
    else
      Log << "(nullopt)";
    Log << DoLog;

    Log << "ClobberedRegisters: {";
    const char *Prefix = "";
    for (GlobalVariable *CSV : ClobberedRegisters.getClobberedRegisters()) {
      Log << Prefix << CSV->getName();
      Prefix = ", ";
    }
    Log << "}" << DoLog;

    Log << "IBIResults:\n";
    for (const auto &[Branch, Edge] : IBIResult) {
      Log << "  " << Branch->Block.toString() << ":\n";
      serialize(Log, Edge);
    }
    Log << DoLog;
  }

  //
  // Commit  IBIResults to the CFG
  //
  for (const auto &[CI, Edge] : IBIResult) {
    efa::BasicBlock &Block = blockFromIndirectBranchInfo(*CI, CFG, Decoded);
    revng_log(Log,
              "Committing the indirect branch of "
                << CI->Block.toString() << " to " << Block.ID().toString());
    Block.Successors().insert(std::move(Edge));
  }

  // Collect summary for information
  using efa::CallEdge;
  bool FoundReturn = false;
  bool FoundBrokenReturn = false;
  int BrokenReturnCount = 0;
  int NoReturnCount = 0;
  for (const auto &[CI, Edge] : IBIResult) {
    using namespace model::FunctionAttribute;
    auto *Call = dyn_cast<CallEdge>(Edge.get());
    if (Edge->Type() == Return) {
      FoundReturn = true;
    } else if (Call != nullptr and Call->IsTailCall()
               and not isNoReturn(*Binary, Oracle, *Call, Recording)) {
      FoundReturn = true;
    } else if (Edge->Type() == BrokenReturn) {
      FoundBrokenReturn = true;
      BrokenReturnCount++;
    } else {
      NoReturnCount++;
    }
  }

  revng_log(Log, "FoundReturn: " << FoundReturn);
  revng_log(Log, "FoundBrokenReturn: " << FoundBrokenReturn);
  revng_log(Log, "BrokenReturnCount: " << BrokenReturnCount);
  revng_log(Log, "NoReturnCount: " << NoReturnCount);

  // Function is elected to inline if there is one and only one broken return
  AttributesSet Attributes;
  if (FoundReturn) {
    // Do nothing
  } else if (FoundBrokenReturn && BrokenReturnCount == 1
             && NoReturnCount == 0) {
    Attributes.insert(model::FunctionAttribute::HasOneBrokenReturn);
  } else {
    Attributes.insert(model::FunctionAttribute::NoReturn);
  }

  for (efa::BasicBlock &Block : CFG) {
    if (Block.Successors().size() == 0)
      revng_log(Log, "Block without successors: " << Block.ID().toString());
    revng_assert(Block.Successors().size() > 0);
  }

  revng_log(Log, "Final CFG:\n");
  if (Log.isEnabled()) {
    LoggerIndent Ident(Log);
    for (const efa::BasicBlock &Block : CFG) {
      serialize(Log, Block);
      Log << DoLog;
    }
  }

  FunctionSummary Result(Attributes,
                         ClobberedRegisters.getClobberedRegisters(),
                         {},
                         std::move(CFG),
                         MaybeWinFSO);

  if (Log.isEnabled()) {
    Log << "Result:\n";
    Result.dump(Log);
    Log << DoLog;
  }

  return Result;
}

/// Describe the size of \p F for a step name: progress traces then tell a
/// large function from a slow one.
static std::string describeSize(const llvm::Function &F) {
  size_t Instructions = 0;
  size_t Calls = 0;
  for (const llvm::BasicBlock &BB : F) {
    for (const llvm::Instruction &I : BB) {
      ++Instructions;
      if (isa<llvm::CallBase>(I))
        ++Calls;
    }
  }

  return " (" + std::to_string(Instructions) + " instructions, "
         + std::to_string(Calls) + " calls, " + std::to_string(F.size())
         + " blocks)";
}

PreparedFunction CFGAnalyzer::prepare(const MetaAddress &Entry,
                                      llvm::Task &T) {
  using namespace llvm;
  using llvm::BasicBlock;

  revng::IRBuilder Builder(M.getContext());
  PreparedFunction Result;
  OutlinedFunction &OutlinedFunction = Result.Outlined;

  // Detect function boundaries
  T.advance("Outline");
  OutlinedFunction = outline(Entry);
  T.advance("Prepare" + describeSize(*OutlinedFunction.Function));

  // Recover the control-flow graph of the function
  Result.CFG = collectDirectCFG(&OutlinedFunction, Result.Decoded);

  BasicBlock &FirstBlock = OutlinedFunction.Function->getEntryBlock();
  if (Result.CFG.size() == 0) {
    // Handle the situation in which the entry block is not in root.
    // This can happen if a model::Function is a non-executable area.

    // Ensure there are not calls to newpc. If there were, it'd be a bug not to
    // have a CFG
    auto IsCallToNewPC = [](Instruction &I) -> bool {
      return NewPCHelper.getCall(&I).has_value();
    };
    revng_assert(none_of(instructions(OutlinedFunction.Function.get()),
                         IsCallToNewPC));
  } else {
    revng_assert(Result.CFG.size() > 0);
  }

  // The analysis aims at identifying the callee-saved registers of a
  // function and establishing if a function returns properly, i.e., it
  // jumps to the return address (regular function). In order to achieve
  // this, the IR is crafted by loading the program counter, the stack
  // pointer, as well as the ABI registers CSVs respectively at function
  // prologue / epilogue. When the subtraction between their entry and end
  // values is found to be zero (after running an LLVM optimization
  // pipeline), we may infer if the function returns correctly, the stack
  // is left unaltered, etc. Hence, upon every original indirect jump
  // (candidate exit point), a marker of this kind is installed:
  //
  //                           jumps to RA,    SP,    rax,   rbx,   rbp
  // call i64 @indirect_branch_info(i128 0, i64 8, i64 %8, i64 0, i64 0)
  //
  // Here, subsequently the opt pipeline computation, we may tell that the
  // function jumps to its return address (thus, it is not a longjmp /
  // tail call), `rax` register has been clobbered by the callee, whereas
  // `rbx` and `rbp` are callee-saved registers.
  createIBIMarker(&OutlinedFunction);

  Function *F = OutlinedFunction.Function.get();

  // Prevent DCE by making branch conditions opaque
  opaqueBranchConditions(F, Builder);

  // Store the values that build up the program counter in order to have them
  // constant-folded away by the optimization pipeline.
  materializePCValues(F, Builder);

  return Result;
}

/// The CSVs the helpers \p F calls read and write, which the pipeline looks up
/// by name
static std::vector<llvm::GlobalValue *> helperCSVs(llvm::Function &F) {
  std::vector<llvm::GlobalValue *> Result;
  for (llvm::Instruction &I : llvm::instructions(F)) {
    if (not isCallToHelper(&I))
      continue;

    if (auto Usage = tryGetCSVUsedByHelperCall(&I)) {
      llvm::append_range(Result, Usage->Read);
      llvm::append_range(Result, Usage->Written);
    }
  }

  return Result;
}

std::vector<IndirectBranchFacts>
CFGAnalyzer::work(const ShippedFunction &Shipped,
                  llvm::StringRef MarkerName,
                  llvm::StringRef StackPointerName,
                  llvm::raw_ostream &OutputIBI,
                  llvm::raw_ostream &OutputAAWriter,
                  llvm::Task &T) {
  using namespace llvm;

  LLVMContext Context;
  T.advance("Parse");
  std::unique_ptr<Module> M = receiveFunction(Shipped, Context);
  Function &F = *M->getFunction(Shipped.Name);
  Function *Marker = M->getFunction(MarkerName);
  GlobalVariable *StackPointer = M->getGlobalVariable(StackPointerName, true);
  revng_assert(Marker != nullptr and StackPointer != nullptr);

  T.advance("Optimize" + describeSize(F));
  runOptimizationPipeline(F, StackPointer, OutputIBI, OutputAAWriter);

  T.advance("Collect results" + describeSize(F));
  return readIndirectBranchInfo(F, *Marker);
}

void CFGAnalyzer::recordReadsIn(PreliminaryAnalysisInputs *Inputs) {
  Recording = Inputs;
  Oracle.recordCallSitesIn(Inputs);
}

ShippedAnalysis CFGAnalyzer::ship(const MetaAddress &Entry, llvm::Task &T) {
  using namespace llvm;

  // Outline and prepare the function, reading the oracle and the model
  PreparedFunction Prepared = prepare(Entry, T);
  Function &Marker = *Prepared.Outlined.IndirectBranchInfoMarker;

  // Move the function to a module of its own and write it as bitcode
  Function *F = Prepared.Outlined.releaseFunction();
  T.advance("Collect helper CSVs" + describeSize(*F));
  // The marker comes along even when the function has no indirect branch
  std::vector<GlobalValue *> Extra = helperCSVs(*F);
  Extra.push_back(&Marker);
  T.advance("Extract" + describeSize(*F));

  ShippedAnalysis Result;
  Result.Address = Prepared.Outlined.Address;
  Result.CFG = std::move(Prepared.CFG);
  Result.Decoded = std::move(Prepared.Decoded);
  Result.Instructions = F->getInstructionCount();
  Result.MarkerName = Marker.getName().str();
  Result.StackPointerName = Globals.spReg()->getName().str();
  Result.Function = shipFunction(*F, Extra);
  return Result;
}

ShippedAnalysis CFGAnalyzer::ship(const MetaAddress &Entry,
                                  PreliminaryAnalysisInputs *Inputs) {
  revng_assert(UnboundedScanLimits::active());
  recordReadsIn(Inputs);
  auto StopRecording = llvm::make_scope_exit([this]() {
    recordReadsIn(nullptr);
  });

  llvm::Task T(4, "Ship function");
  return ship(Entry, T);
}

std::vector<IndirectBranchFacts>
CFGAnalyzer::runShipped(const ShippedAnalysis &Analysis) {
  revng_assert(canRunInParallel());
  llvm::Task T(3, "Analyze shipped function");
  return work(Analysis.Function,
              Analysis.MarkerName,
              Analysis.StackPointerName,
              llvm::nulls(),
              llvm::nulls(),
              T);
}

FunctionSummary
CFGAnalyzer::finish(ShippedAnalysis &&Analysis,
                    const std::vector<IndirectBranchFacts> &Branches,
                    PreliminaryAnalysisInputs *Inputs) {
  recordReadsIn(Inputs);
  auto StopRecording = llvm::make_scope_exit([this]() {
    recordReadsIn(nullptr);
  });

  return milkInfo(Analysis.Address,
                  std::move(Analysis.CFG),
                  Analysis.Decoded,
                  Branches);
}

bool CFGAnalyzer::canRunInParallel() {
  return AAWriterPath.getNumOccurrences() == 0
         and IndirectBranchInfoSummaryPath.getNumOccurrences() == 0;
}

FunctionSummary CFGAnalyzer::analyze(const MetaAddress &Entry,
                                     PreliminaryAnalysisInputs *Inputs) {
  using namespace llvm;

  revng_assert(UnboundedScanLimits::active());

  // Record what this analysis reads, here and through the oracle
  recordReadsIn(Inputs);
  auto StopRecording = llvm::make_scope_exit([this]() {
    recordReadsIn(nullptr);
  });

  llvm::Task T(RoundTrip ? 7 : 4, "Analyze function");

  if (RoundTrip) {
    // Run the pipeline on a copy of the function parsed from bitcode into a
    // context of its own
    ShippedAnalysis Shipped = ship(Entry, T);
    std::vector<IndirectBranchFacts> Branches = work(Shipped.Function,
                                                     Shipped.MarkerName,
                                                     Shipped.StackPointerName,
                                                     *OutputIBI,
                                                     *OutputAAWriter,
                                                     T);
    return milkInfo(Shipped.Address,
                    std::move(Shipped.CFG),
                    Shipped.Decoded,
                    Branches);
  }

  // Outline and prepare the function, reading the oracle and the model
  PreparedFunction Prepared = prepare(Entry, T);
  Function &Marker = *Prepared.Outlined.IndirectBranchInfoMarker;

  // Execute the optimization pipeline over the outlined function, which reads
  // nothing but the function and its module
  Function &F = *Prepared.Outlined.Function;
  T.advance("Optimize" + describeSize(F));
  runOptimizationPipeline(F, Globals.spReg(), *OutputIBI, *OutputAAWriter);

  // Squeeze out the results obtained from the optimization passes
  T.advance("Collect results" + describeSize(F));
  std::vector<IndirectBranchFacts>
    Branches = readIndirectBranchInfo(F, Marker);

  return milkInfo(Prepared.Outlined.Address,
                  std::move(Prepared.CFG),
                  Prepared.Decoded,
                  Branches);
}

CallSummarizer::CallSummarizer(llvm::Module *M,
                               Function *PreCallHook,
                               Function *PostCallHook,
                               llvm::Function *RetHook,
                               GlobalVariable *SPCSV,
                               llvm::SmallSet<MetaAddress, 4> *ReturnBlocks) :
  M(M),
  PreCallHook(PreCallHook),
  PostCallHook(PostCallHook),
  RetHook(RetHook),
  SPCSV(SPCSV),
  ReturnBlocks(ReturnBlocks),
  Clobberer(M) {
}

void CallSummarizer::handleCall(MetaAddress CallerBlock,
                                revng::IRBuilder &Builder,
                                MetaAddress Callee,
                                const CSVSet &ClobberedRegisters,
                                const std::optional<int64_t> &MaybeFSO,
                                bool IsNoReturn,
                                bool IsTailCall,
                                llvm::Value *SymbolNamePointer) {
  using namespace llvm;
  LLVMContext &Context = getContext(M);

  // Mark end of basic block with a pre-hook call
  Value *IsTailCallValue = ConstantInt::getBool(Context, IsTailCall);

  SmallVector<Value *, 4> Args = { CallerBlock.toValue(M),
                                   Callee.toValue(M),
                                   SymbolNamePointer,
                                   IsTailCallValue };

  Module *M = Builder.GetInsertBlock()->getParent()->getParent();

  Builder.CreateCall(PreCallHook, Args);

  // Emulate registers being clobbered
  for (GlobalVariable *Register : ClobberedRegisters)
    Clobberer.clobber(Builder, Register);

  // Adjust back the stack pointer
  if (not IsTailCall) {
    if (MaybeFSO.has_value()) {
      auto *StackPointer = Builder.createLoad(SPCSV);
      Value *Offset = ConstantInt::get(StackPointer->getType(), *MaybeFSO);
      auto *AdjustedStackPointer = Builder.CreateAdd(StackPointer, Offset);
      Builder.CreateStore(AdjustedStackPointer, SPCSV);
    }
  }

  // Mark end of basic block with a post-hook call
  Builder.CreateCall(PostCallHook, Args);
}

void CallSummarizer::handlePostNoReturn(revng::IRBuilder &Builder,
                                        const llvm::DebugLoc &DbgLocation) {
  emitAbort(Builder, "", DbgLocation);
}

void CallSummarizer::handleIndirectJump(revng::IRBuilder &Builder,
                                        MetaAddress Block,
                                        const CSVSet &ClobberedRegisters,
                                        llvm::Value *SymbolNamePointer) {
  bool EmitCallHook = true;

  if (ReturnBlocks != nullptr) {
    bool IsReturn = ReturnBlocks->count(Block) != 0;
    EmitCallHook = not IsReturn;
  }

  CallInst *NewPreCallHook = nullptr;
  CallInst *NewPostCallHook = nullptr;
  if (EmitCallHook) {
    NewPreCallHook = Builder.CreateCall(PreCallHook,
                                        { Block.toValue(M),
                                          MetaAddress::invalid().toValue(M),
                                          SymbolNamePointer,
                                          Builder.getTrue() });

    // Emulate registers being clobbered
    for (GlobalVariable *Register : ClobberedRegisters)
      Clobberer.clobber(Builder, Register);

    NewPostCallHook = Builder.CreateCall(PostCallHook,
                                         { Block.toValue(M),
                                           MetaAddress::invalid().toValue(M),
                                           SymbolNamePointer,
                                           Builder.getTrue() });
  }

  Builder.CreateCall(RetHook, { Block.toValue(M) });

  if (EmitCallHook) {
    NewPreCallHook->getParent()->splitBasicBlockBefore(NewPreCallHook);
    auto *PostPostCallHook = NewPostCallHook->getNextNode();
    NewPostCallHook->getParent()->splitBasicBlockBefore(PostPostCallHook);
  }
}

} // namespace efa
