//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <chrono>
#include <compare>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/CodeGen/UnreachableBlockElim.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRPrinter/IRPrintingPasses.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Progress.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "revng/ABI/FunctionType/Layout.h"
#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/BasicAnalyses/ShrinkInstructionOperandsPass.h"
#include "revng/FunctionCallIdentification/FunctionCallIdentification.h"
#include "revng/Model/ABI/Definition.h"
#include "revng/Support/FunctionCallMarker.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/NewPC.h"
#include "revng/Support/OpaqueRegisterUser.h"
#include "revng/Support/Statistics.h"
#include "revng/TypeShrinking/BitLiveness.h"
#include "revng/TypeShrinking/TypeShrinking.h"
#include "revng/ValueMaterializer/DataFlowGraph.h"

#include "JumpTargetManager.h"
#include "NoReturnAnalysis.h"
#include "RootAnalyzer.h"
#include "ValueMaterializerPass.h"

using namespace llvm;

RunningStatistics BlocksAnalyzedByValueMaterializer("blocks-analyzed-by-avi");
RunningStatistics WrittenInPCStatistics("written-in-pc");
RunningStatistics DetectedEdgesStatistics("detected-edges");
RunningStatistics StoredInMemoryStatistics("stored-in-memory");
RunningStatistics LoadAddressStatistics("load-address");

Logger NewEdgesLog("new-edges");
static Logger Log("root-analyzer");
static Logger JumpTableExtentLog("jump-table-extent");
static Logger HarvestRegionLog("harvest-region");

static cl::opt<bool> StopAtFunctionEntries("harvest-stop-at-function-entries",
                                           cl::desc("When choosing the part "
                                                    "of root a harvest round "
                                                    "optimizes, do not walk "
                                                    "backward past a callee "
                                                    "or a function symbol "
                                                    "(experimental)"),
                                           cl::init(false));

// NOTE: Setting this to 1 gives us performance improvement. We have tested and
// realized that there is an impact on performance if setting it to 2.
constexpr unsigned InstCombineMaxIterations = 1;

/// Drop all the call to marker functions
class DropMarkerCalls : public PassInfoMixin<DropMarkerCalls> {
private:
  SmallVector<StringRef, 4> NoReturns;

public:
  DropMarkerCalls(SmallVector<StringRef, 4> NoReturns) : NoReturns(NoReturns) {}

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    Module *M = F.getParent();
    std::vector<CallBase *> ToErase;

    for (Function &Marker : FunctionTags::Marker.functions(M)) {
      StringRef MarkerName = Marker.getName();
      if (llvm::count(NoReturns, MarkerName) == 0) {
        for (CallBase *Call : callersIn(&Marker, &F)) {
          // Register the call to be erased
          ToErase.push_back(Call);
        }
      }
    }

    //
    // Actually drop the calls
    //
    for (CallBase *Call : ToErase)
      eraseFromParent(Call);

    return PreservedAnalyses::none();
  }
};

/// Simple pass to drop `range` metadata, which is sometimes detrimental
class DropRangeMetadataPass : public PassInfoMixin<DropRangeMetadataPass> {

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        I.setMetadata("range", nullptr);
    return PreservedAnalyses::all();
  }
};

/// Turn load instructions from constant addresses into constants
class ConstantLoadsFolderPass : public PassInfoMixin<ConstantLoadsFolderPass> {
private:
  StaticDataMemoryOracle &MO;

public:
  ConstantLoadsFolderPass(StaticDataMemoryOracle &MO) : MO(MO) {}

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    // TODO: we shouldn't create new functions in a function pass
    auto *BoolType = llvm::IntegerType::getInt1Ty(F.getContext());
    auto *RandomBoolType = FunctionType::get(BoolType, {});
    FunctionCallee RandomBoolCallee = F.getParent()
                                        ->getOrInsertFunction("random_bool",
                                                              RandomBoolType);
    Function *RandomBool = cast<Function>(RandomBoolCallee.getCallee());
    RandomBool->setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
    RandomBool->addFnAttr(Attribute::NoUnwind);
    RandomBool->addFnAttr(Attribute::WillReturn);

    for (Instruction &I : llvm::instructions(F)) {
      auto *Load = dyn_cast<LoadInst>(&I);
      if (Load == nullptr)
        continue;

      Type *LoadType = Load->getType();
      if (not LoadType->isIntegerTy())
        continue;

      ConstantInt *Address = nullptr;
      using namespace PatternMatch;
      if (not match(Load, m_Load(m_IntToPtr(m_ConstantInt(Address)))))
        continue;

      uint64_t LoadAddress = Address->getLimitedValue();
      unsigned LoadSize = LoadType->getIntegerBitWidth() / 8;
      MaterializedValue Loaded = MO.load(LoadAddress, LoadSize);
      if (not Loaded.isValid() or Loaded.hasSymbol())
        continue;

      auto *ConstantValue = ConstantInt::get(LoadType, Loaded.value());

      if (Loaded.isMutable()) {
        IRBuilder<> Builder(Load->getNextNode());
        auto *ReplaceWith = Builder.CreateSelect(Builder.CreateCall(RandomBool),
                                                 Load,
                                                 ConstantValue);
        Load->replaceAllUsesWith(ReplaceWith);

        // Fix the second operand of the select
        cast<SelectInst>(ReplaceWith)->setOperand(1, Load);

      } else {
        Load->replaceAllUsesWith(ConstantValue);
      }
    }
    return PreservedAnalyses::none();
  }
};

// LLVM with the fork's LazyValueInfo patch (lvi-cache-contents) defines these;
// stock LLVM does not, and the type shrinking counters then omit the cache.
namespace llvm {
__attribute__((weak)) size_t
getLazyValueInfoCacheContents(const LazyValueInfo &LVI,
                              SmallVectorImpl<Value *> &TrackedValues);
__attribute__((weak)) void eraseLazyValueInfoValue(LazyValueInfo &LVI,
                                                    Value *V);
} // namespace llvm

/// What a harvest round's type shrinking would cost if JumpThreading's
/// LazyValueInfo cache were kept: each tracked value that type shrinking
/// deletes or replaces makes the cache visit every cached block. The cache is
/// dropped before type shrinking, so these counters measure the work it avoids
/// and how it grows with the temporary root, and report it in the trace.
struct TypeShrinkingScaling {
  /// Counts the first deletion or replacement of its value, the event on which
  /// LazyValueInfo would erase that value
  class RemovalCounter final : public CallbackVH {
  private:
    size_t *Count;

  public:
    RemovalCounter(Value *V, size_t &Count) : CallbackVH(V), Count(&Count) {}

    void deleted() override {
      ++*Count;
      setValPtr(nullptr);
    }

    void allUsesReplacedWith(Value *) override { deleted(); }
  };

  std::vector<RemovalCounter> Counters;
  size_t Removed = 0;
  bool Measured = false;
};

/// Erases this many tracked values from the cache, evenly spaced over them, to
/// time one erasure. The cache is dropped right after, so only entries about
/// to be discarded go.
static constexpr size_t SampledErasures = 16;

/// Reports the temporary root's size and the LazyValueInfo cache that
/// JumpThreading leaves, and starts counting the tracked values that type
/// shrinking removes. It changes nothing but the cache, which the next pass
/// drops.
class MeasureLazyValueInfoPass
  : public PassInfoMixin<MeasureLazyValueInfoPass> {
private:
  TypeShrinkingScaling &Scaling;

public:
  MeasureLazyValueInfoPass(TypeShrinkingScaling &Scaling) : Scaling(Scaling) {}

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    Task T(1, "Lazy value info before type shrinking");
    std::string Description = std::to_string(F.size()) + " blocks, "
                              + std::to_string(F.getInstructionCount())
                              + " instructions";

    auto *LVI = FAM.getCachedResult<LazyValueAnalysis>(F);
    bool Available = getLazyValueInfoCacheContents != nullptr
                     and eraseLazyValueInfoValue != nullptr;
    if (LVI != nullptr and Available) {
      SmallVector<Value *, 0> Tracked;
      size_t CachedBlocks = getLazyValueInfoCacheContents(*LVI, Tracked);

      Scaling.Measured = true;
      Scaling.Counters.reserve(Tracked.size());
      for (Value *V : Tracked)
        Scaling.Counters.emplace_back(V, Scaling.Removed);

      size_t Sampled = std::min(SampledErasures, Tracked.size());
      using Clock = std::chrono::steady_clock;
      auto Start = Clock::now();
      for (size_t I = 0; I < Sampled; ++I)
        eraseLazyValueInfoValue(*LVI, Tracked[I * Tracked.size() / Sampled]);
      auto Elapsed = Clock::now() - Start;
      auto Nanoseconds = std::chrono::nanoseconds(Elapsed).count();

      Description += ", " + std::to_string(CachedBlocks) + " cached blocks, "
                     + std::to_string(Tracked.size()) + " tracked values, "
                     + std::to_string(Sampled) + " erases in "
                     + std::to_string(Nanoseconds) + " ns";
    }

    T.advance(Description);
    return PreservedAnalyses::all();
  }
};

/// Reports how many of the tracked values type shrinking removed, and stops
/// counting
class CountTypeShrinkingRemovalsPass
  : public PassInfoMixin<CountTypeShrinkingRemovalsPass> {
private:
  TypeShrinkingScaling &Scaling;

public:
  CountTypeShrinkingRemovalsPass(TypeShrinkingScaling &Scaling) :
    Scaling(Scaling) {}

public:
  PreservedAnalyses run(Function &, FunctionAnalysisManager &) {
    if (Scaling.Measured) {
      Task T(1, "Type shrinking removals");
      T.advance(std::to_string(Scaling.Removed) + " tracked values removed");
    }
    Scaling = {};
    return PreservedAnalyses::all();
  }
};

namespace TrackedInstructionType {

enum Values {
  Invalid,
  WrittenInPC,
  StoredInMemory,
  StoreTarget,
  LoadTarget
};

inline const char *getName(Values V) {
  switch (V) {
  case Invalid:
    return "Invalid";
  case WrittenInPC:
    return "WrittenInPC";
  case StoredInMemory:
    return "StoredInMemory";
  case StoreTarget:
    return "StoreTarget";
  case LoadTarget:
    return "LoadTarget";
  default:
    revng_abort();
  }
}

inline Values fromName(llvm::StringRef Name) {
  if (Name == "Invalid")
    return Invalid;
  else if (Name == "WrittenInPC")
    return WrittenInPC;
  else if (Name == "StoredInMemory")
    return StoredInMemory;
  else if (Name == "StoreTarget")
    return StoreTarget;
  else if (Name == "LoadTarget")
    return LoadTarget;
  else
    revng_abort();
}

} // namespace TrackedInstructionType

class AnalysisRegistry {
public:
  using TrackedValueType = TrackedInstructionType::Values;

  struct TrackedValue {
    MetaAddress Address;
    TrackedValueType Type;
    Instruction *I = nullptr;
  };

private:
  std::vector<TrackedValue> TrackedValues;
  QuickMetadata QMD;
  llvm::Function *ValueMaterializerMarker;
  revng::IRBuilder Builder;

public:
  AnalysisRegistry(Module *M) : QMD(getContext(M)), Builder(getContext(M)) {
    ValueMaterializerMarker = ValueMaterializerPass::createMarker(*M);
  }

  llvm::Function *aviMarker() const { return ValueMaterializerMarker; }

  void registerValue(MetaAddress Address,
                     Value *OriginalValue,
                     Value *ValueToTrack,
                     TrackedValueType Type) {
    revng_assert(Address.isValid());

    Instruction *InstructionToTrack = dyn_cast<Instruction>(ValueToTrack);
    if (InstructionToTrack == nullptr)
      return;

    revng_assert(InstructionToTrack != nullptr);

    unsigned MaxPhiLike = DataFlowGraph::Limits::Max;
    unsigned MaxLoad = DataFlowGraph::Limits::Max;
    Oracle::Values Oracle = Oracle::AdvancedValueInfo;

    // Configure ValueMaterializer
    switch (Type) {
    case TrackedInstructionType::WrittenInPC:
      // We do not think it's beneficial to traverse more than one load
      MaxLoad = 1;
      break;
    case TrackedInstructionType::LoadTarget:
    case TrackedInstructionType::StoreTarget:
      // The main goal of tracking load/store addresses is to detect constant
      // pools between functions. We deem it hard that the address of a constant
      // pool is loaded from memory.
      MaxLoad = 0;
      break;
    case TrackedInstructionType::StoredInMemory:
      // We do not think it's beneficial to traverse more than one load
      MaxLoad = 1;

      // Tracking this values is a version of collecting simple literals on
      // steroids. Basically we want to just track constant merged through a
      // phi. No need for LazyValueInfo or AdvancedValueInfo.
      Oracle = Oracle::None;
      break;
    default:
      revng_abort();
    }

    // Create the marker call and attach as last argument a unique
    // identifier. This is necessary since the instruction itself could be
    // deleted, duplicated and what not. Later on, we will use TrackedValues
    // to now the values that have been identified to which value in the
    // original function did they belong to
    llvm::Module *M = ValueMaterializerMarker->getParent();
    uint32_t ValueMaterializerID = TrackedValues.size();
    Builder.SetInsertPoint(InstructionToTrack->getNextNode());
    Builder.CreateCall(ValueMaterializerMarker,
                       { InstructionToTrack,
                         Builder.getInt32(MaxPhiLike),
                         Builder.getInt32(MaxLoad),
                         Builder.getInt32(Oracle),
                         getUniqueString(M, Address.toString()),
                         Builder.getInt32(ValueMaterializerID) });
    TrackedValue NewTV{ Address,
                        Type,
                        cast_or_null<Instruction>(OriginalValue) };
    TrackedValues.push_back(NewTV);
  }

  const TrackedValue &rootInstructionById(uint32_t ID) const {
    return TrackedValues.at(ID);
  }
};

RootAnalyzer::RootAnalyzer(JumpTargetManager &JTM) :
  JTM(JTM), TheModule(JTM.module()), Model(JTM.model()) {
}

RootAnalyzer::MetaAddressSet RootAnalyzer::inflateValueMaterializerWhitelist() {
  MetaAddressSet Result;
  InflationSeeds = 0;

  // We start from all the new basic blocks (i.e., those in
  // ValueMaterializerPCWhiteList) and proceed backward in the CFG in order to
  // whitelist all the jump targets we meet. We stop when we meet the dispatcher
  // or a function call.

  // Prepare the backward visit
  df_iterator_default_set<BasicBlock *> VisitSet;

  // Stop at the dispatcher
  VisitSet.insert(JTM.dispatcher());

  // TODO: OriginalInstructionAddresses is not reliable, we should drop it
  std::optional NewPCFunction = NewPCHelper.get(TheModule);
  revng_assert(NewPCFunction.has_value());
  for (IRHelperCall<NewPCArgument> NewPCCall : NewPCFunction->callers()) {
    auto *I = NewPCCall.call();
    MetaAddress WhitelistedMA = addressFromNewPC(NewPCCall);
    if (WhitelistedMA.isValid()) {
      if (JTM.isInValueMaterializerPCWhitelist(WhitelistedMA)) {
        BasicBlock *BB = I->getParent();
        ++InflationSeeds;
        if (StopAtFunctionEntries) {
          // The same visit, except that the predecessors of a function entry
          // are not visited: they reach it through a tail call
          SmallVector<BasicBlock *, 16> Worklist;
          if (VisitSet.insert(BB).second)
            Worklist.push_back(BB);
          while (not Worklist.empty()) {
            BasicBlock *Reachable = Worklist.pop_back_val();
            auto MA = getBasicBlockAddress(Reachable);
            bool IsEntry = false;
            if (MA.isValid() and JTM.isJumpTarget(MA)) {
              Result.insert(MA);
              const auto &Target = JTM.at(MA);
              IsEntry = Target.hasReason(JumpTargetReason::Callee)
                        or Target.hasReason(JumpTargetReason::FunctionSymbol);
            }
            if (IsEntry)
              continue;
            for (BasicBlock *Predecessor : predecessors(Reachable))
              if (VisitSet.insert(Predecessor).second)
                Worklist.push_back(Predecessor);
          }
          continue;
        }
        auto VisitRange = inverse_depth_first_ext(BB, VisitSet);
        for (const BasicBlock *Reachable : VisitRange) {
          auto MA = getBasicBlockAddress(Reachable);
          if (MA.isValid() and JTM.isJumpTarget(MA)) {
            Result.insert(MA);
          }
        }
      }
    }
  }

  InflationVisited = VisitSet.size();
  return Result;
}

static llvm::SmallSet<model::Register::Values, 16>
getPreservedRegisters(const model::TypeDefinition &Prototype) {
  llvm::SmallSet<model::Register::Values, 16> Result;
  namespace FT = abi::FunctionType;
  for (model::Register::Values Register : FT::calleeSavedRegisters(Prototype))
    Result.insert(Register);
  return Result;
}

/// Collect the indirect branches whose analysis \ref cutNoReturnFallthroughs
/// has invalidated.
///
/// Detaching an edge can only remove paths, so the only program points whose
/// reaching definitions change are those downstream of it: walking forward from
/// the detached fallthroughs gives exactly that set.
static std::set<MetaAddress>
collectInvalidatedIndirectBranches(JumpTargetManager &JTM,
                                   Function &Root,
                                   ArrayRef<BasicBlock *>
                                     DetachedFallthroughs) {
  std::set<MetaAddress> Result;

  if (DetachedFallthroughs.empty())
    return Result;

  DenseMap<BasicBlock *, MetaAddress> IndirectBranches;
  for (CallBase *Call : callersIn(JTM.exitTB(), &Root))
    IndirectBranches[Call->getParent()] = getPC(Call).first;

  SmallPtrSet<BasicBlock *, 32> Visited;
  SmallVector<BasicBlock *, 16> WorkList;

  // Do not escape through the dispatcher, the only block from which the whole
  // program is reachable
  Visited.insert(JTM.dispatcher());

  for (BasicBlock *BB : DetachedFallthroughs)
    if (Visited.insert(BB).second)
      WorkList.push_back(BB);

  while (not WorkList.empty()) {
    BasicBlock *BB = WorkList.pop_back_val();

    auto It = IndirectBranches.find(BB);
    if (It != IndirectBranches.end() and It->second.isValid())
      Result.insert(It->second);

    for (BasicBlock *Successor : successors(BB))
      if (Visited.insert(Successor).second)
        WorkList.push_back(Successor);
  }

  return Result;
}

// Clone the root function.
Function *RootAnalyzer::createTemporaryRoot(Function *TheFunction,
                                            ValueToValueMapTy &OldToNew) {
  Function *OptimizedFunction = nullptr;
  Module *M = TheFunction->getParent();
  // Break all the call edges. We want to ignore those for CFG recovery
  // purposes.
  llvm::DenseSet<BasicBlock *> Callees;
  llvm::DenseMap<Use *, BasicBlock *> Undo;
  auto *FunctionCall = functionOrNull(FunctionCallMarker.get(TheModule));
  if (FunctionCall) {
    for (CallBase *Call : callers(FunctionCall)) {
      auto *T = Call->getParent()->getTerminator();

      Callees.insert(getFunctionCallCallee(Call->getParent()));

      if (auto *Branch = dyn_cast<BranchInst>(T)) {
        revng_assert(Branch->isUnconditional());
        BasicBlock *Target = Branch->getSuccessor(0);
        Use *U = &Branch->getOperandUse(0);

        BasicBlock *Fallthrough = nullptr;
        if (not isa<ConstantPointerNull>(Call->getArgOperand(0)))
          Fallthrough = getFallthrough(Call->getParent());

        U->set(Fallthrough != nullptr ? Fallthrough : JTM.anyPC());

        // Record Use for later undoing
        Undo[U] = Target;
      }
    }
  }

  // Compute ValueMaterializerJumpTargetWhitelist
  JumpTargetManager::WhitelistOriginCounts Origins = JTM.whitelistOrigins();
  size_t WhitelistedPCs = JTM.whitelistSize();
  auto
    ValueMaterializerJumpTargetWhitelist = inflateValueMaterializerWhitelist();

  // Prune the dispatcher
  JTM.setCFGForm(CFGForm::NoFunctionCalls,
                 &ValueMaterializerJumpTargetWhitelist);

  // Detach the fallthrough of calls that never return
  auto DetachedFallthroughs = cutNoReturnFallthroughs(*TheFunction,
                                                      *Model,
                                                      JTM.dispatcher());

  // Whatever we concluded downstream of those fallthroughs was concluded on a
  // CFG we now know to be wrong. Collect the indirect branches involved, so we
  // can ask for them to be looked at again.
  auto Invalidated = collectInvalidatedIndirectBranches(JTM,
                                                        *TheFunction,
                                                        DetachedFallthroughs);

  // Detach all the unreachable basic blocks, so they don't get copied
  llvm::DenseSet<BasicBlock *> UnreachableBBs = JTM.computeUnreachable();
  for (BasicBlock *UnreachableBB : UnreachableBBs)
    UnreachableBB->removeFromParent();

  // Report what the temporary root is made of: the whitelist by origin, the
  // backward visit from it, the jump targets it whitelists, and how much of
  // the root stays reachable from them.
  revng_log(HarvestRegionLog,
            "whitelist " << WhitelistedPCs << " (new targets "
                         << Origins.NewTargets << ", new branches "
                         << Origins.NewBranches << ", invalidated "
                         << Origins.Invalidated << "), seeds "
                         << InflationSeeds << ", backward visited "
                         << InflationVisited << ", whitelisted targets "
                         << ValueMaterializerJumpTargetWhitelist.size()
                         << " of " << JTM.jumpTargetsCount()
                         << ", detached fallthroughs "
                         << DetachedFallthroughs.size() << ", invalidated "
                         << Invalidated.size() << ", root blocks "
                         << TheFunction->size() << ", unreachable "
                         << UnreachableBBs.size());

  // Clone the function
  OptimizedFunction = CloneFunction(TheFunction, OldToNew);

  // Restore callees after function_call
  for (auto &&[U, BB] : Undo)
    U->set(BB);

  // Force canonical register values at the beginning of each callee
  Callees.erase(nullptr);
  revng::IRBuilder Builder(TheModule.getContext());
  for (BasicBlock *BB : Callees) {
    if (OldToNew.count(BB) == 0)
      continue;
    BB = cast<BasicBlock>(OldToNew[BB]);
    revng_assert(BB->getTerminator() != nullptr);
    Builder.SetInsertPoint(BB->getFirstNonPHI());

    for (const model::Segment &Segment : Model->Segments()) {
      if (Segment.contains(getBasicBlockAddress(BB))) {
        for (const auto &CanonicalValue : Segment.CanonicalRegisterValues()) {
          uint64_t C = model::Register::getCSVCount(CanonicalValue.Register());
          if (C != 1) {
            revng_log(Log,
                      "Ignoring canonical value for register "
                        << model::Register::getName(CanonicalValue.Register())
                        << ": it is composed of " << C << " CSVs");
            continue;
          }

          auto Name = model::Register::singleCSVName(CanonicalValue.Register());
          if (auto *CSV = M->getGlobalVariable(Name)) {
            auto *Type = getCSVType(CSV);
            Builder.CreateStore(ConstantInt::get(Type, CanonicalValue.Value()),
                                CSV);
          }
        }
        break;
      }
    }
  }

  //
  // Turn function_call into clobbering non-callee-saved registers
  //
  {
    // Compute preserved registers using the default prototype
    using RegisterSet = llvm::SmallSet<model::Register::Values, 16>;
    RegisterSet PreservedRegisters;
    model::ABI::Values ABI = Model->DefaultABI();
    if (const auto *DefaultPrototype = Model->defaultPrototype()) {
      // TODO: don't forget to simplify the logic here if we decide to make
      //       default prototypes always available (now that `abi::Definition`
      //       is a part of the model).
      PreservedRegisters = getPreservedRegisters(*DefaultPrototype);
    } else if (ABI != model::ABI::Invalid) {
      auto &CSRs = abi::Definition::get(ABI).CalleeSavedRegisters();
      PreservedRegisters.insert(CSRs.begin(), CSRs.end());
    } else {
      // TODO: this must be a preliminary check
      revng_abort("Either DefaultABI or DefaultPrototype needs to be "
                  "specified");
    }

    OpaqueRegisterUser Clobberer(M);
    SmallVector<CallBase *, 16> FunctionCallCalls;

    if (FunctionCall != nullptr) {
      llvm::copy(callersIn(FunctionCall, OptimizedFunction),
                 std::back_inserter(FunctionCallCalls));
    }

    for (CallBase *Call : FunctionCallCalls) {
      Builder.SetInsertPoint(Call);

      // Clobber registers that are not preserved
      for (model::Register::Values Register :
           model::Architecture::registers(Model->Architecture())) {
        if (not PreservedRegisters.contains(Register))
          Clobberer.clobber(Builder, Register);
      }
    }

    for (CallBase *Call : FunctionCallCalls)
      Call->eraseFromParent();
  }

  // Record the size of OptimizedFunction
  size_t BlocksCount = OptimizedFunction->size();
  BlocksAnalyzedByValueMaterializer.push(BlocksCount);

  // Reattach the unreachable basic blocks to the original root function
  for (BasicBlock *UnreachableBB : UnreachableBBs)
    UnreachableBB->insertInto(TheFunction);

  // Restore the dispatcher in the original function
  JTM.setCFGForm(CFGForm::SemanticPreserving);
  revng_assert(JTM.computeUnreachable().size() == 0);

  // Clear the whitelist
  JTM.clearValueMaterializerPCWhitelist();

  JTM.recordDetachedFallthroughs(DetachedFallthroughs, Invalidated);

  return OptimizedFunction;
}

// Helper to intrinsic promotion
void RootAnalyzer::promoteHelpersToIntrinsics(Function *OptimizedFunction,
                                              revng::IRBuilder &Builder) {
  using MapperFunction = std::function<Instruction *(CallInst *)>;
  std::pair<std::vector<StringRef>, MapperFunction> Mapping[] = {
    { { "helper_clz", "helper_clz32", "helper_clz64", "helper_dclz" },
      [&Builder](CallInst *Call) {
        return Builder.CreateBinaryIntrinsic(Intrinsic::ctlz,
                                             Call->getArgOperand(0),
                                             Builder.getFalse());
      } }
  };

  for (auto &[HelperNames, Mapper] : Mapping) {
    for (StringRef HelperName : HelperNames) {
      if (Function *Original = TheModule.getFunction(HelperName)) {

        SmallVector<std::pair<Instruction *, Instruction *>, 16> Replacements;
        for (User *U : Original->users()) {
          if (auto *Call = dyn_cast<CallInst>(U)) {
            if (Call->getParent()->getParent() == OptimizedFunction) {
              Builder.SetInsertPoint(Call);
              Instruction *NewI = Mapper(Call);
              NewI->copyMetadata(*Call);
              Replacements.emplace_back(Call, NewI);
            }
          }
        }

        // Apply replacements
        for (auto &P : Replacements) {
          P.first->replaceAllUsesWith(P.second);
          eraseFromParent(P.first);
        }
      }
    }
  }
}

RootAnalyzer::GlobalToAllocaTy
RootAnalyzer::promoteCSVsToAlloca(Function *OptimizedFunction) {
  GlobalToAllocaTy CSVMap;

  // Collect all the non-PC affecting CSVs
  DenseSet<GlobalVariable *> CSVs;
  for (GlobalVariable &CSV : FunctionTags::CSV.globals(&TheModule))
    CSVs.insert(&CSV);

  for (GlobalVariable *CSV : JTM.programCounterHandler()->pcCSVs())
    CSVs.insert(CSV);

  // Create and initialize an alloca per CSV (except for the PC-affecting ones)
  BasicBlock *EntryBB = &OptimizedFunction->getEntryBlock();
  revng::IRBuilder AllocaBuilder(&*EntryBB->begin());
  revng::IRBuilder InitBuilder(EntryBB->getTerminator());

  for (GlobalVariable *CSV : toSortedByName(CSVs)) {
    Type *CSVType = CSV->getValueType();
    auto *Alloca = AllocaBuilder.CreateAlloca(CSVType, nullptr, CSV->getName());
    CSVMap[CSV] = Alloca;

    // Replace all uses of the CSV within OptimizedFunction with the alloca
    replaceAllUsesInFunctionWith(OptimizedFunction, CSV, Alloca);

    // Initialize the alloca
    InitBuilder.CreateStore(InitBuilder.createLoad(CSV), Alloca);
  }

  return CSVMap;
}

SummaryCallsBuilder RootAnalyzer::optimize(llvm::Function *OptimizedFunction,
                                           const Features &CommonFeatures) {
  using namespace model::Architecture;
  using namespace model::Register;

  Module *M = OptimizedFunction->getParent();

  auto CSVMap = promoteCSVsToAlloca(OptimizedFunction);

  SummaryCallsBuilder SCB(CSVMap);

  // Put together information about syscalls
  StringRef SyscallHelperName = getSyscallHelper(Model->Architecture());
  Function *SyscallHelper = M->getFunction(SyscallHelperName);
  auto SyscallIDRegister = getSyscallNumberRegister(Model->Architecture());
  StringRef SyscallIDCSVName = getName(SyscallIDRegister);
  GlobalVariable *SyscallIDCSV = M->getGlobalVariable(SyscallIDCSVName);

  // Remove PC initialization from entry block: this is required otherwise the
  // dispatcher will be constant-propagated away
  {
    BasicBlock &Entry = OptimizedFunction->getEntryBlock();
    std::vector<Instruction *> ToDelete;
    for (Instruction &I : Entry)
      if (auto *Store = dyn_cast<StoreInst>(&I))
        if (isa<Constant>(Store->getValueOperand())
            and JTM.programCounterHandler()->affectsPC(Store))
          ToDelete.push_back(&I);

    for (Instruction *I : ToDelete)
      eraseFromParent(I);
  }

  // The StaticDataMemoryOracle provide the contents of memory areas that are
  // mapped statically (i.e., in segments). This is critical to capture, e.g.,
  // virtual tables
  StaticDataMemoryOracle MO(TheModule.getDataLayout(), JTM, CommonFeatures);

  {
    // Note: it is important to let the pass manager go out of scope ASAP:
    //       LazyValueInfo registers a lot of callbacks to get notified when a
    //       Value is destroyed, slowing down OptimizedFunction->eraseFromParent
    //       enormously.

    // The order of the passes, when and how many times they are run are
    // inspired by the -O2 pipeline. You can see it in action as follows:
    //
    //       clang test.c -emit-llvm -o- -Xclang -disable-O0-optnone | \
    //         opt -O2 -S -debug-pass-manager

    FunctionPassManager FPM;

    // Drop all markers except exitTB
    FPM.addPass(DropMarkerCalls({ "exitTB" }));

    // Summarize calls to helpers
    FPM.addPass(DropHelperCallsPass(SyscallHelper, SyscallIDCSV, SCB));

    // TODO: do we still need this?
    FPM.addPass(ShrinkInstructionOperandsPass());

    // Canonicalization
    FPM.addPass(PromotePass());
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(InstCombinePass(InstCombineMaxIterations));

    // This ensures we have in the IR values from constant pools, which will
    // then get collected by collectValuesStoredIntoMemory
    FPM.addPass(ConstantLoadsFolderPass(MO));

    // Running JumpThreading is important to merge multiple instructions with
    // the same predicate in a single "if" (in particular in ARM) and obtain
    // more accurate constraints.
    FPM.addPass(JumpThreadingPass());
    TypeShrinkingScaling Scaling;
    FPM.addPass(MeasureLazyValueInfoPass(Scaling));
    // No other pass after JumpThreading depends or uses LVI.
    // LazyValueAnalysis registers deletion callbacks that get called by
    // following modifications of the IR. The cost of calling these callbacks
    // can get very high, and nobody will use the results anyway.
    // So let's invalidate LazyValueAnalysis straight after JT.
    FPM.addPass(InvalidateAnalysisPass<LazyValueAnalysis>());

    // Shrink instructions

    // InstCombine should not run after TypeShrinking since it undoes its work.
    // Specifically it turns icmp that have been shrank to 32-bit by
    // TypeShrinking back to 64-bits.
    FPM.addPass(TypeShrinking::TypeShrinkingPass());
    FPM.addPass(CountTypeShrinkingRemovalsPass(Scaling));

    // It is important to run EarlyCSE after TypeShrinking to factor trunc
    // instructions and have more accurate constraints.
    FPM.addPass(EarlyCSEPass(true));

    FPM.addPass(SimplifyCFGPass());

    // Drop range metadata
    FPM.addPass(DropRangeMetadataPass());

    // Run ValueMaterializer!
    FPM.addPass(ValueMaterializerPass(MO));

    FunctionAnalysisManager FAM;
    FAM.registerPass([]() { return TypeShrinking::BitLivenessPass(); });
    FAM.registerPass([] {
      AAManager AA;
      AA.registerFunctionAnalysis<BasicAA>();
      AA.registerFunctionAnalysis<ScopedNoAliasAA>();

      return AA;
    });

    ModuleAnalysisManager MAM;
    auto MAMFunactionProxyFactory = [&MAM] {
      return ModuleAnalysisManagerFunctionProxy(MAM);
    };
    FAM.registerPass(MAMFunactionProxyFactory);

    // The pass manager reports each pass as a step; report each analysis run,
    // such as bit liveness, as a subtask of the pass that requested it
    std::vector<std::unique_ptr<Task>> Analyses;
    PassInstrumentationCallbacks PIC;
    PIC.registerBeforeAnalysisCallback([&Analyses](StringRef Name, Any) {
      Analyses.push_back(std::make_unique<Task>(std::nullopt, Name));
    });
    PIC.registerAfterAnalysisCallback([&Analyses](StringRef, Any) {
      Analyses.pop_back();
    });

    PassBuilder PB(nullptr, PipelineTuningOptions(), std::nullopt, &PIC);
    PB.registerFunctionAnalyses(FAM);
    PB.registerModuleAnalyses(MAM);

    FPM.run(*OptimizedFunction, FAM);
  }

  return SCB;
}

void RootAnalyzer::collectMaterializedValues(AnalysisRegistry &AR) {
  auto &Context = TheModule.getContext();
  QuickMetadata QMD(Context);

  // Iterate over all the ValueMaterializer markers
  Function *ValueMaterializerMarker = AR.aviMarker();

  // TODO: we could use a better data structure here
  struct Entry {
    StringRef SymbolName;
    ConstantInt *Value = nullptr;
    /// The lowest address of a table entry the value was read from, if every
    /// time it was read by a single load
    std::optional<uint64_t> TableEntry;

    bool operator==(const Entry &Other) const {
      return SymbolName == Other.SymbolName and Value == Other.Value;
    }

    // Order by value, not by the address of the uniqued constant: the order
    // decides the order in which new jump targets, and their blocks, are
    // created
    bool operator<(const Entry &Other) const {
      if (SymbolName != Other.SymbolName)
        return SymbolName < Other.SymbolName;

      const APInt &LHS = Value->getValue();
      const APInt &RHS = Other.Value->getValue();
      if (LHS.getBitWidth() != RHS.getBitWidth())
        return LHS.getBitWidth() < RHS.getBitWidth();

      return LHS.ult(RHS);
    }
  };
  std::map<uint32_t, SmallVector<Entry>> MaterializedValuesById;
  // The instructions value materialization produced nothing for
  std::set<uint32_t> Unmaterialized;

  // Collect materialized values
  // Note: there could be multiple calls to the marker with the same ID
  for (CallBase *Call : callers(ValueMaterializerMarker)) {
    revng_log(Log, "collectMaterializedValues on " << getName(Call));

    // Get the ID from the marker, and then the original instruction and marker
    // type
    Value *LastArgument = Call->getArgOperand(Call->arg_size() - 1);
    uint32_t ValueMaterializerID = getLimitedValue(LastArgument);

    // Did ValueMaterializer produce any info?
    auto *T = dyn_cast_or_null<MDTuple>(Call->getMetadata("revng.avi"));
    if (T == nullptr) {
      Unmaterialized.insert(ValueMaterializerID);
      continue;
    }

    auto &Operands = MaterializedValuesById[ValueMaterializerID];
    for (const MDOperand &Operand : cast<MDTuple>(T)->operands()) {
      // Extract the value
      auto *Tuple = QMD.extract<MDTuple *>(Operand.get());
      auto SymbolName = QMD.extract<StringRef>(Tuple->getOperand(0).get());
      auto *Value = QMD.extract<ConstantInt *>(Tuple->getOperand(1).get());
      std::optional<uint64_t> TableEntry;
      if (Tuple->getNumOperands() > 2) {
        auto *Entry = QMD.extract<ConstantInt *>(Tuple->getOperand(2).get());
        TableEntry = getLimitedValue(Entry);
      }
      Operands.push_back({ SymbolName, Value, TableEntry });
    }
  }

  // A table indexed by a value whose range the program does not check, such as
  // a byte, makes value materialization read past its end: the entries that
  // follow belong to other tables and point into other functions. When each
  // destination of the indirect jump at Jump was read from a table entry,
  // keep the destinations of the entries before the first one that does not
  // point to an instruction of the jump's function or that lies at or past
  // another address the function's code refers to. If not even the first one
  // does, the table is not one of this function's (for instance, a table of
  // functions) and all of them are kept.
  auto BoundJumpTable = [this](MetaAddress Jump, SmallVector<Entry> &Operands) {
    if (Operands.empty())
      return;

    for (const Entry &Operand : Operands)
      if (Operand.SymbolName.size() != 0 or not Operand.TableEntry)
        return;

    SmallVector<const Entry *> ByTableEntry;
    for (const Entry &Operand : Operands)
      ByTableEntry.push_back(&Operand);
    llvm::sort(ByTableEntry, [](const Entry *LHS, const Entry *RHS) {
      return *LHS->TableEntry < *RHS->TableEntry;
    });

    SmallVector<MetaAddress> Targets;
    SmallVector<uint64_t> Entries;
    for (const Entry *Operand : ByTableEntry) {
      Targets.push_back(MetaAddress::decomposeIntegerPC(Operand->Value));
      Entries.push_back(*Operand->TableEntry);
    }

    auto Extent = JTM.unwindRanges().tableExtent(Jump, Targets, Entries);
    if (not Extent or *Extent == 0 or *Extent == Targets.size())
      return;

    revng_log(JumpTableExtentLog,
              "Jump at " << Jump.toString() << ": keeping " << *Extent
                         << " of " << Targets.size()
                         << " destinations, first dropped "
                         << Targets[*Extent].toString());

    uint64_t FirstDropped = *ByTableEntry[*Extent]->TableEntry;
    llvm::erase_if(Operands, [FirstDropped](const Entry &Operand) {
      return *Operand.TableEntry >= FirstDropped;
    });
  };

  // A jump through a table whose index the analysis cannot bound gets no
  // values. When its instructions are the usual switch dispatch, read the table
  // and keep the entries that lead into the jump's function.
  for (auto &[ID, Operands] : MaterializedValuesById)
    if (Operands.empty())
      Unmaterialized.insert(ID);
  for (uint32_t ValueMaterializerID : Unmaterialized) {
    auto TV = AR.rootInstructionById(ValueMaterializerID);
    if (TV.Type != TrackedInstructionType::WrittenInPC)
      continue;
    auto Table = JTM.unwindRanges().relativeJumpTable(TV.Address);
    if (not Table)
      continue;

    revng_log(JumpTableExtentLog,
              "Jump at " << TV.Address.toString() << ": " << Table->first.size()
                         << " destinations read from its table");
    revng_assert(MaterializedValuesById[ValueMaterializerID].empty());
    auto &Operands = MaterializedValuesById[ValueMaterializerID];
    for (auto [Target, TableEntry] : llvm::zip(Table->first, Table->second)) {
      APInt Composed(128, Target.address());
      Composed |= APInt(128, Target.epoch()) << 64;
      Composed |= APInt(128, Target.addressSpace()) << (64 + 32);
      Composed |= APInt(128, Target.type()) << (64 + 32 + 16);
      Operands.push_back(
          {StringRef(), ConstantInt::get(Context, Composed), TableEntry});
    }
  }

  for (auto &[ValueMaterializerID, Operands] : MaterializedValuesById) {
    revng_log(Log, "Parsing ID " << ValueMaterializerID);
    LoggerIndent Indent(Log);

    // Remove duplicates, keeping the lowest table entry each value was read
    // from
    llvm::sort(Operands);
    SmallVector<Entry> UniqueOperands;
    for (const Entry &Operand : Operands) {
      if (UniqueOperands.empty() or not(UniqueOperands.back() == Operand)) {
        UniqueOperands.push_back(Operand);
        continue;
      }

      std::optional<uint64_t> &Kept = UniqueOperands.back().TableEntry;
      if (Kept and Operand.TableEntry)
        Kept = std::min(*Kept, *Operand.TableEntry);
      else
        Kept = std::nullopt;
    }
    Operands = std::move(UniqueOperands);

    auto TV = AR.rootInstructionById(ValueMaterializerID);
    auto TIT = TV.Type;
    Instruction *I = TV.I;

    revng_log(Log, TrackedInstructionType::getName(TIT));

    if (TIT == TrackedInstructionType::WrittenInPC)
      BoundJumpTable(TV.Address, Operands);

    // Is this a direct write to PC?
    bool IsComposedIntegerPC = (TIT == TrackedInstructionType::WrittenInPC);

    // We want to register the results only if *all* of them are good
    bool AllValid = true;
    bool AllPCs = true;

    SmallVector<MetaAddress, 16> Targets;

    // Iterate over all the generated values
    for (const auto &[SymbolName, Value, TableEntry] : Operands) {
      bool HasDynamicSymbol = SymbolName.size() != 0;
      if (not HasDynamicSymbol) {
        // Deserialize value into a MetaAddress, depending on the tracked
        // instruction type
        auto MA = (IsComposedIntegerPC ?
                     MetaAddress::decomposeIntegerPC(Value) :
                     MetaAddress::fromPC(TV.Address, getLimitedValue(Value)));

        if (MA.isInvalid()) {
          AllValid = false;
        } else {
          if (not JTM.isPC(MA)) {
            revng_log(Log,
                      "The following address is not a PC: " << MA.toString());
            AllPCs = false;
          }

          Targets.push_back(MA);
        }
      }
    }

    if (Log.isEnabled()) {
      Log << "Targets:\n";
      for (const MetaAddress &Target : Targets)
        Log << "  " << Target << "\n";
      Log << DoLog;
    }

    // Proceed only if all the results are valid
    if (not AllValid) {
      revng_log(Log, "Not all targets are valid, ignoring.");
      continue;
    }

    // If it's supposed to be a PC, all of them have to be a PC
    bool ShouldBePC = (TIT == TrackedInstructionType::WrittenInPC
                       or TIT == TrackedInstructionType::StoredInMemory);
    if (ShouldBePC and not AllPCs) {
      revng_log(Log,
                "All targets were expected to point to code, but some don't.");
      continue;
    }

    // Register the resulting addresses
    unsigned RegisteredAddresses = 0;

    auto RegisterJT = [this,
                       &RegisteredAddresses](MetaAddress Address,
                                             JumpTargetReason::Values Reason) {
      bool IsNew = not JTM.hasJT(Address);
      if (JTM.registerJT(Address, Reason) != nullptr and IsNew) {
        ++RegisteredAddresses;
      }
    };

    switch (TIT) {
    case TrackedInstructionType::WrittenInPC:
      for (const MetaAddress &MA : Targets)
        RegisterJT(MA, JumpTargetReason::PCStore);
      WrittenInPCStatistics.push(RegisteredAddresses);
      break;

    case TrackedInstructionType::StoredInMemory:
      for (const MetaAddress &MA : Targets)
        RegisterJT(MA, JumpTargetReason::MemoryStore);
      StoredInMemoryStatistics.push(RegisteredAddresses);
      break;

    case TrackedInstructionType::StoreTarget:
    case TrackedInstructionType::LoadTarget:
      for (const MetaAddress &MA : Targets)
        if (JTM.markJT(MA, JumpTargetReason::LoadAddress))
          ++RegisteredAddresses;
      LoadAddressStatistics.push(RegisteredAddresses);
      break;

    case TrackedInstructionType::Invalid:
      revng_abort();
    }

    if (TIT == TrackedInstructionType::WrittenInPC) {
      // This is a call to `exit_tb`, transfer the revng.avi metadata on the
      // call as revng.targets for later processing
      revng_assert(TV.I != nullptr);

      // Compose the metadata for revng.targets
      SmallVector<Metadata *> NewOperands;
      for (const auto &[SymbolName, Value, TableEntry] : Operands)
        NewOperands.push_back(QMD.tuple({ QMD.get(SymbolName),
                                          QMD.get(Value) }));

      TV.I->setMetadata("revng.targets", MDTuple::get(Context, NewOperands));
      DetectedEdgesStatistics.push(Targets.size());
      revng_log(NewEdgesLog,
                Targets.size() << " targets from #" << ValueMaterializerID);
    }
  }
}

using JTM2 = RootAnalyzer;

void JTM2::collectValuesStoredIntoMemory(Function *F,
                                         const Features &CommonFeatures) {
  for (Instruction &I : llvm::instructions(F)) {
    if (auto *Store = dyn_cast<StoreInst>(&I)) {
      auto *Pointer = Store->getPointerOperand();
      auto *Address = dyn_cast<ConstantInt>(Store->getValueOperand());
      if (isMemory(Pointer) and Address != nullptr
          and JTM.programCounterHandler()->isPCSizedType(Address->getType())) {
        auto MA = MetaAddress::fromPC(Address->getLimitedValue(),
                                      CommonFeatures);
        if (MA.isValid()) {
          JTM.registerJT(MA, JumpTargetReason::MemoryStore);
        }
      }
    }
  }
}

static MetaAddress::Features findCommonFeatures(Function *F) {
  bool First = true;
  MetaAddress::Features Result;
  std::optional NewPC = NewPCHelper.get(*F->getParent());
  revng_assert(NewPC.has_value());
  for (IRHelperCall<NewPCArgument> NewPCCall : NewPC->callersIn(F)) {
    MetaAddress Address = addressFromNewPC(NewPCCall);

    if (First) {
      Result = Address.features();
    } else {
      // TODO: once we switch to multi-binary run ValueMaterializer once per
      //       each feature set
      revng_assert(Result == Address.features());
    }
  }

  return Result;
}

void RootAnalyzer::cloneOptimizeAndHarvest(Function *TheFunction) {
  Task T(7, "Harvest with value materialization");
  T.advance("Identify function calls");

  // Re-run the identification of function calls
  RootFunction Root(*TheFunction->getParent());
  CSVGlobals Globals(*Model, *TheFunction->getParent());
  legacy::PassManager PM;
  PM.add(new FunctionCallIdentification(Root, Globals, Model->Architecture()));
  PM.run(TheModule);

  T.advance("Create temporary root");

  std::optional<ValueToValueMapTy> OldToNew(std::in_place);
  Function *OptimizedFunction = createTemporaryRoot(TheFunction, *OldToNew);

  MetaAddress::Features CommonFeatures = findCommonFeatures(OptimizedFunction);

  AnalysisRegistry AR(&TheModule);

  // The temporary root holds only the part of the root reaching new code, so
  // its size bounds the work of the rest of the round. Report it in each
  // remaining step name so progress monitors can tell a marginal round.
  size_t Blocks = OptimizedFunction->size();
  size_t Exits = 0;
  auto Size = [&]() {
    std::string Result = " (" + std::to_string(Blocks) + " blocks";
    if (Exits != 0)
      Result += ", " + std::to_string(Exits) + " exits";
    return Result + ")";
  };

  T.advance("Register exit values" + Size());

  // Register for analysis the value written in the PC before each exit_tb call
  revng::IRBuilder Builder(TheModule.getContext());
  for (CallBase *Call : callersIn(JTM.exitTB(), TheFunction)) {
    BasicBlock *BB = Call->getParent();
    auto It = OldToNew->find(Call);
    if (It == OldToNew->end())
      continue;
    Builder.SetInsertPoint(cast<CallInst>(&*It->second));
    ProgramCounterHandler *PCH = JTM.programCounterHandler();
    Instruction *ComposedIntegerPC = PCH->composeIntegerPC(Builder);
    AR.registerValue(getPC(Call).first,
                     Call,
                     ComposedIntegerPC,
                     TrackedInstructionType::WrittenInPC);
    ++Exits;
  }

  T.advance("Prepare temporary root" + Size());

  // The map holds a tracking handle on every original and cloned value, which
  // makes each later deletion or replacement in the clone update it. Nothing
  // reads it after this point, so release it before optimizing.
  OldToNew.reset();

  promoteHelpersToIntrinsics(OptimizedFunction, Builder);

  // Replace calls to newpc with stores to the PC
  SmallVector<CallBase *, 16> ToErase;
  std::optional NewPC = NewPCHelper.get(TheModule);
  revng_assert(NewPC.has_value());
  for (IRHelperCall<NewPCArgument> Call : NewPC->callersIn(OptimizedFunction)) {
    JTM.programCounterHandler()->expandNewPC(Call);
    ToErase.push_back(Call.call());
  }

  for (CallBase *Call : ToErase)
    eraseFromParent(Call);

  T.advance("Optimize temporary root" + Size());

  // Optimize the hell out of it and collect the possible values of indirect
  // branches.
  auto SCB = optimize(OptimizedFunction, CommonFeatures);

  revng::verify(OptimizedFunction);

  T.advance("Collect materialized values" + Size());

  // Collect the results
  collectMaterializedValues(AR);

  // Collect pointer-sized values being stored in memory
  collectValuesStoredIntoMemory(OptimizedFunction, CommonFeatures);

  T.advance("Destroy temporary root" + Size());

  // Drop the optimized function
  eraseFromParent(OptimizedFunction);

  // Drop temporary functions
  SCB.cleanup();
}
