//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include "revng/InlineHelpers/InlineHelpers.h"
#include "revng/Support/CommandLine.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/Tag.h"

using namespace llvm;

static Logger Log("inline-helpers");
static Logger CacheLog("helper-specialization-cache");

static cl::opt<std::string> DumpBeforeDir("inline-helpers-dump-before-dir",
                                          cl::desc("Directory in which to "
                                                   "write one `<fn>.ll` file "
                                                   "per Isolated function, "
                                                   "containing the body of "
                                                   "the function right "
                                                   "before inline-helpers "
                                                   "touches it. Created if "
                                                   "absent."),
                                          cl::init(""),
                                          cl::cat(MainCategory));

static cl::opt<std::string> DumpAfterDir("inline-helpers-dump-after-dir",
                                         cl::desc("Directory in which to "
                                                  "write one `<fn>.ll` file "
                                                  "per Isolated function, "
                                                  "containing the body of "
                                                  "the function right after "
                                                  "inline-helpers has "
                                                  "finished with it. "
                                                  "Created if absent."),
                                         cl::init(""),
                                         cl::cat(MainCategory));

// Dump one `<F.name>.ll` file to `Dir`, containing just the body of `F`.
// Called once per Isolated function per pass invocation, from
// `InlineHelpers::run(F)`; the user then gets exactly one file per
// Isolated function, named by it.
static void dumpFunctionToDir(const Function &F, StringRef Dir) {
  std::error_code EC = sys::fs::create_directories(Dir);
  revng_assert(not EC,
               ("inline-helpers: cannot create dump directory: " + EC.message())
                 .c_str());

  SmallString<256> Path(Dir);
  sys::path::append(Path, (F.getName() + ".ll").str());

  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  revng_assert(not EC,
               ("inline-helpers: cannot open dump file: " + EC.message())
                 .c_str());
  F.print(OS, nullptr);
}

char InlineHelpersLegacyPass::ID = 0;

using Register = RegisterPass<InlineHelpersLegacyPass>;
static Register X("inline-helpers", "Inline Helpers Pass", true, true);

/// Loops in a specialized helper with a constant trip count up to this bound
/// are unrolled completely, so that per-iteration CPU-state offsets become
/// constants.
static constexpr unsigned MaxUnrolledTripCount = 64;

/// Copies of helpers specialized for the constant arguments at their call
/// sites.
///
/// Helpers reach CPU-state variables through a switch over every byte offset
/// relative to `@env`. The module defines `@env` and the critical arguments
/// are constants, so each switch folds to one case once loops over bytes or
/// lanes are unrolled. Inlining the original body instead copies every case
/// into the caller: a single SSE shuffle helper can exceed 300,000
/// instructions. Each specialization is built once and reused by every call
/// with the same helper and constant arguments.
///
/// Every function of a binary gets its own module with its own copy of the
/// helpers, so the per-module cache alone rebuilds the same specializations
/// once per function. Built specializations are therefore also kept for the
/// whole process, as bitcode keyed by a description of the helper, the
/// module's `@env` and the constant arguments, and are cloned into later
/// modules instead of being rebuilt.
class HelperSpecializations {
private:
  using Key = std::pair<Function *, std::vector<Constant *>>;
  std::map<Key, Function *> Cache;

  /// Specializations built in earlier modules: bitcode of a module whose one
  /// definition is the specialization, or no value when specializing did not
  /// make the helper smaller
  static std::map<std::string, std::optional<std::string>> Shared;

public:
  ~HelperSpecializations() {
    for (auto &[Key, Specialized] : Cache)
      if (Specialized != nullptr)
        Specialized->eraseFromParent();
  }

  /// Replace \p Call with a call to its helper specialized for the call's
  /// constant arguments, unless specializing does not make the helper
  /// smaller.
  CallInst *specialize(CallInst *Call);

private:
  static Function *build(Function *Helper, ArrayRef<Constant *> Arguments);
  static std::string sharedKey(Function *Helper,
                               ArrayRef<Constant *> Arguments);
  static std::string serialize(Function *Specialized);
  static Function *materialize(const std::string &Bitcode,
                               Function *Helper);
};

std::map<std::string, std::optional<std::string>>
  HelperSpecializations::Shared;

/// Describe everything a specialization depends on that can differ between
/// modules: the helper, the `@env` offset it folds against, the data layout
/// and the constant arguments
std::string HelperSpecializations::sharedKey(Function *Helper,
                                             ArrayRef<Constant *> Arguments) {
  Module *M = Helper->getParent();
  std::string Result;
  raw_string_ostream OS(Result);
  OS << Helper->getName() << "|" << *Helper->getFunctionType() << "|"
     << Helper->getInstructionCount() << "|" << M->getDataLayoutStr() << "|";
  if (auto *Env = M->getNamedGlobal("env"); Env and Env->hasInitializer())
    Env->getInitializer()->printAsOperand(OS, true, M);
  for (Constant *Argument : Arguments) {
    OS << "|";
    if (Argument == nullptr)
      OS << "_";
    else
      Argument->printAsOperand(OS, true, M);
  }
  return OS.str();
}

/// Bitcode of a module holding \p Specialized and declarations of what it
/// references
std::string HelperSpecializations::serialize(Function *Specialized) {
  ValueToValueMapTy VMap;
  auto Copy = CloneModule(*Specialized->getParent(),
                          VMap,
                          [Specialized](const GlobalValue *GV) {
                            return GV == Specialized;
                          });
  std::string Result;
  raw_string_ostream OS(Result);
  WriteBitcodeToFile(*Copy, OS);
  return OS.str();
}

/// Clone the specialization in \p Bitcode into the module of \p Helper,
/// binding each global it references to the same-named global of that
/// module. The bitcode also declares every other global of the module it was
/// built in; only those the specialization uses are bound. Return nullptr, so
/// the caller builds the specialization itself, if a used global is missing
/// or has a different type.
Function *HelperSpecializations::materialize(const std::string &Bitcode,
                                             Function *Helper) {
  Module &M = *Helper->getParent();
  auto Parsed = parseBitcodeFile(MemoryBufferRef(Bitcode, "specialization"),
                                 M.getContext());
  revng_assert(Parsed, "cannot parse a cached helper specialization");
  Module &Source = **Parsed;

  Function *Original = nullptr;
  for (Function &F : Source)
    if (not F.isDeclaration())
      Original = &F;
  revng_assert(Original != nullptr);

  ValueToValueMapTy VMap;
  for (GlobalValue &Global : Source.global_values()) {
    if (&Global == Original or Global.use_empty())
      continue;
    GlobalValue *Target = M.getNamedValue(Global.getName());
    if (Target == nullptr or Target->getValueType() != Global.getValueType()) {
      revng_log(CacheLog,
                "cannot bind " << Global.getName() << " for "
                               << Helper->getName());
      return nullptr;
    }
    VMap[&Global] = Target;
  }

  Function *Result = Function::Create(Original->getFunctionType(),
                                      GlobalValue::InternalLinkage,
                                      Helper->getName() + ".specialized",
                                      M);
  for (auto [From, To] : zip(Original->args(), Result->args()))
    VMap[&From] = &To;
  SmallVector<ReturnInst *, 4> Returns;
  CloneFunctionInto(Result,
                    Original,
                    VMap,
                    CloneFunctionChangeType::DifferentModule,
                    Returns);
  return Result;
}

/// Run \p FPM on \p F with every analysis available, including the module
/// proxies that alias analysis requires.
static void runPasses(Function &F, FunctionPassManager &FPM) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  FPM.run(F, FAM);
}

static void simplify(Function &F, bool UnrollLoops) {
  FunctionPassManager FPM;
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  if (UnrollLoops) {
    // Rotation moves each loop's exit test to its latch, where scalar
    // evolution computes the constant trip count
    FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(LCSSAPass());
  }
  runPasses(F, FPM);
}

/// Unroll completely every innermost loop with a small constant trip count,
/// simplifying after each, until none remains.
static void unrollConstantLoops(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  while (true) {
    simplify(F, true);

    DominatorTree DT(F);
    LoopInfo LI(DT);
    AssumptionCache AC(F);
    TargetLibraryInfoImpl TLII(Triple(F.getParent()->getTargetTriple()));
    TargetLibraryInfo TLI(TLII, &F);
    ScalarEvolution SE(F, TLI, AC, DT, LI);
    TargetTransformInfo TTI(DL);
    OptimizationRemarkEmitter ORE(&F);

    Loop *ToUnroll = nullptr;
    unsigned TripCount = 0;
    for (Loop *L : LI.getLoopsInPreorder()) {
      if (not L->isInnermost() or not L->isLoopSimplifyForm())
        continue;
      // A loop with an early exit has only a bound, which also allows a
      // complete unroll that keeps each iteration's exit test
      unsigned Count = SE.getSmallConstantTripCount(L);
      if (Count == 0)
        Count = SE.getSmallConstantMaxTripCount(L);
      if (Count > 0 and Count <= MaxUnrolledTripCount) {
        ToUnroll = L;
        TripCount = Count;
        break;
      }
    }
    if (ToUnroll == nullptr)
      return;

    UnrollLoopOptions ULO;
    ULO.Count = TripCount;
    ULO.Force = true;
    ULO.Runtime = false;
    ULO.AllowExpensiveTripCount = false;
    ULO.UnrollRemainder = false;
    ULO.ForgetAllSCEV = true;
    auto Result = UnrollLoop(ToUnroll, ULO, &LI, &SE, &DT, &AC, &TTI, &ORE, true);
    if (Result != LoopUnrollResult::FullyUnrolled)
      return;
  }
}

Function *HelperSpecializations::build(Function *Helper,
                                        ArrayRef<Constant *> Arguments) {
  ValueToValueMapTy VMap;
  for (auto [Argument, Value] : zip(Helper->args(), Arguments))
    if (Value != nullptr)
      VMap[&Argument] = Value;

  // Arguments mapped to constants are dropped from the clone's signature
  Function *Specialized = CloneFunction(Helper, VMap);
  Specialized->setName(Helper->getName() + ".specialized");
  Specialized->setLinkage(GlobalValue::InternalLinkage);
  Specialized->setSection("");
  Specialized->removeFnAttr(Attribute::NoInline);

  unrollConstantLoops(*Specialized);

  FunctionPassManager FPM;
  FPM.addPass(SCCPPass());
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  FPM.addPass(ADCEPass());
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  runPasses(*Specialized, FPM);

  unsigned Before = Helper->getInstructionCount();
  unsigned After = Specialized->getInstructionCount();
  revng_log(Log,
            "specialized " << Helper->getName() << ": " << Before << " -> "
                           << After << " instructions");

  // Unrolling can make a small helper larger: keep the original then
  if (After > Before) {
    Specialized->eraseFromParent();
    return nullptr;
  }

  return Specialized;
}

CallInst *HelperSpecializations::specialize(CallInst *Call) {
  Function *Helper = getCalledFunction(Call);
  std::vector<Constant *> Arguments;
  for (Value *Argument : Call->args())
    Arguments.push_back(dyn_cast<Constant>(Argument));

  auto [It, New] = Cache.try_emplace({ Helper, Arguments }, nullptr);
  if (New) {
    std::string Key = sharedKey(Helper, Arguments);
    auto SharedIt = Shared.find(Key);
    revng_log(CacheLog,
              (SharedIt == Shared.end() ? "miss " : "hit ") << Key);
    if (SharedIt == Shared.end()) {
      It->second = build(Helper, Arguments);
      Shared[Key] = It->second == nullptr ?
                      std::nullopt :
                      std::optional(serialize(It->second));
    } else if (SharedIt->second.has_value()) {
      It->second = materialize(*SharedIt->second, Helper);
      if (It->second == nullptr) {
        revng_log(Log, "rebuilding " << Helper->getName() << " locally");
        It->second = build(Helper, Arguments);
      }
    }
  }
  if (It->second == nullptr)
    return Call;

  SmallVector<Value *, 4> Remaining;
  for (auto [Argument, Value] : zip(Call->args(), Arguments))
    if (Value == nullptr)
      Remaining.push_back(Argument);

  auto *Specialized = CallInst::Create(It->second, Remaining, "", Call);
  Specialized->setDebugLoc(Call->getDebugLoc());
  Call->replaceAllUsesWith(Specialized);
  Call->eraseFromParent();
  return Specialized;
}

class InlineHelpers {
private:
  HelperSpecializations &Specializations;

public:
  InlineHelpers(HelperSpecializations &Specializations) :
    Specializations(Specializations) {}

  void run(Function *F);

private:
  void doInline(CallInst *Call) const;
  bool doInline(Function *F) const;
  CallInst *getCallToInline(Instruction *I) const;
  bool shouldInline(const CallInst *Call) const;
};

bool InlineHelpers::shouldInline(const CallInst *Call) const {
  const Function *Callee = getCalledFunction(Call);
  if (Callee == nullptr)
    return false;

  if (not isInlineHelper(*Callee))
    return false;

  InliningPolicy P = deserializeInliningPolicy(*Callee);
  const BitVector &Critical = P.CriticalArguments;

  if (Critical.none()) {
    revng_log(Log, "inline " << Callee->getName() << ": no critical arguments");
    return true;
  }

  revng_log(Log, "Analyzing arguments of " << Callee->getName());
  LoggerIndent Indent(Log);

  bool AllConstant = true;
  for (int Index = Critical.find_first(); Index >= 0;
       Index = Critical.find_next(Index)) {
    const bool IsConst = isa<Constant>(Call->getArgOperand(Index));
    if (not IsConst)
      AllConstant = false;
    revng_log(Log,
              "argument " << Index << ": "
                          << (IsConst ? "const" : "NON-CONST"));
  }

  revng_log(Log, (AllConstant ? "inline" : "skip"));
  return AllConstant;
}

CallInst *InlineHelpers::getCallToInline(Instruction *I) const {
  if (auto *Call = dyn_cast<CallInst>(I)) {
    if (shouldInline(Call)) {
      return Call;
    }
  }

  return nullptr;
}

void InlineHelpers::doInline(CallInst *Call) const {
  Call = Specializations.specialize(Call);
  InlineFunctionInfo IFI;
  auto Result = InlineFunction(*Call, IFI, false, nullptr, false);
  revng_assert(Result.isSuccess(), Result.getFailureReason());
}

bool InlineHelpers::doInline(Function *F) const {
  SmallVector<CallInst *, 8> ToInline;

  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (auto *Call = getCallToInline(&I))
        ToInline.push_back(Call);

  for (CallInst *Call : ToInline)
    doInline(Call);

  return ToInline.size() > 0;
}

static void dropDebugOrPseudoInst(Function *F) {
  SmallVector<Instruction *, 16> ToErase;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (I.isDebugOrPseudoInst()) {
        ToErase.push_back(&I);
      }
    }
  }

  for (Instruction *I : ToErase) {
    I->eraseFromParent();
  }
}

void InlineHelpers::run(Function *F) {

  if (F->isDeclaration())
    return;

  revng_log(Log, "Analyzing " << F->getName());

  // Dump the IR of the function before the inlining takes place
  if (not DumpBeforeDir.empty())
    dumpFunctionToDir(*F, DumpBeforeDir);

  // `slim-down-helpers-module` already fully inlines helpers, but we might
  // still have wrappers created by `promote-csvs` at runtime that we need to
  // traverse and inline as well.
  while (doInline(F)) {
  }
  dropDebugOrPseudoInst(F);

  // Dump the IR of the function after inlining has taken place
  if (not DumpAfterDir.empty())
    dumpFunctionToDir(*F, DumpAfterDir);
}

void inlineHelpers(llvm::Module &M) {
  // Inline `revng_inline` helper calls into the `Isolated` functions, where the
  // per-call critical arguments are constant at the call site.
  SmallVector<Function *, 32> Isolated;
  for (Function &F : M)
    if (FunctionTags::Isolated.isTagOf(&F))
      Isolated.push_back(&F);

  HelperSpecializations Specializations;
  llvm::Task T(Isolated.size(), "Inline helpers");
  for (Function *F : Isolated) {
    T.advance(F->getName());
    InlineHelpers IH(Specializations);
    IH.run(F);
  }
}

llvm::PreservedAnalyses InlineHelpersPass::run(llvm::Module &M,
                                               llvm::ModuleAnalysisManager &) {
  inlineHelpers(M);
  return llvm::PreservedAnalyses::none();
}

bool InlineHelpersLegacyPass::runOnModule(llvm::Module &M) {
  inlineHelpers(M);
  return true;
}
