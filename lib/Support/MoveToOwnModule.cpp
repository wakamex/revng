/// \file MoveToOwnModule.cpp
/// Move a function to a module of its own and through bitcode.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/MoveToOwnModule.h"

using namespace llvm;

static cl::opt<bool> VerifyMoved("verify-moved-functions",
                                 cl::desc("Verify every module a function is "
                                          "moved to and every module parsed "
                                          "from a shipped function"),
                                 cl::init(false));

static Logger Log("move-to-own-module");

/// Arguments, blocks and instructions of \p F, in a fixed order, and the
/// instructions alone
static void collectValues(Function &F,
                          std::vector<Value *> &Values,
                          std::vector<Instruction *> &Instructions) {
  for (Argument &A : F.args())
    Values.push_back(&A);
  for (BasicBlock &BB : F) {
    Values.push_back(&BB);
    for (Instruction &I : BB) {
      Values.push_back(&I);
      Instructions.push_back(&I);
    }
  }
}

static FunctionUseListOrder recordUseListOrder(Function &F) {
  FunctionUseListOrder Result;
  std::vector<Value *> Values;
  std::vector<Instruction *> Instructions;
  collectValues(F, Values, Instructions);

  DenseMap<const User *, unsigned> Index;
  for (unsigned I = 0; I < Instructions.size(); ++I)
    Index[Instructions[I]] = I;

  auto UseOf = [&Index](const Use &U) -> std::pair<unsigned, unsigned> {
    auto It = Index.find(U.getUser());
    return { It == Index.end() ? ~0U : It->second, U.getOperandNo() };
  };

  for (unsigned I = 0; I < Values.size(); ++I) {
    if (not Values[I]->hasNUsesOrMore(2))
      continue;

    FunctionUseListOrder::UseList Uses;
    for (const Use &U : Values[I]->uses())
      Uses.push_back(UseOf(U));
    Result.Locals.emplace_back(I, std::move(Uses));
  }

  // A global's use list also holds the uses of the rest of the module. Walk
  // it only as far as it holds uses by F, which are usually first, since they
  // are the newest
  MapVector<GlobalValue *, unsigned> UsesOfGlobals;
  for (Instruction *I : Instructions)
    for (Use &Operand : I->operands())
      if (auto *Global = dyn_cast<GlobalValue>(Operand.get()))
        ++UsesOfGlobals[Global];

  for (auto &&[Global, Count] : UsesOfGlobals) {
    if (Count < 2 or not Global->hasName())
      continue;

    FunctionUseListOrder::UseList Uses;
    for (const Use &U : Global->uses()) {
      auto Entry = UseOf(U);
      if (Entry.first == ~0U)
        break;
      Uses.push_back(Entry);
      if (Uses.size() == Count)
        break;
    }

    if (Uses.size() == Count) {
      Result.Globals.emplace_back(Global->getName().str(), std::move(Uses));
    } else {
      revng_log(Log,
                "The uses of " << Global->getName().str() << " by "
                               << F.getName().str()
                               << " are not first in its use list");
    }
  }

  return Result;
}

static void applyUseListOrder(Function &F, const FunctionUseListOrder &Order) {
  std::vector<Value *> Values;
  std::vector<Instruction *> Instructions;
  collectValues(F, Values, Instructions);

  DenseMap<const Use *, unsigned> Rank;
  auto Sort = [&](Value *V, const FunctionUseListOrder::UseList &Uses) {
    Rank.clear();
    for (unsigned I = 0; I < Uses.size(); ++I) {
      auto [User, Operand] = Uses[I];
      if (User != ~0U)
        Rank[&Instructions.at(User)->getOperandUse(Operand)] = I;
    }

    auto RankOf = [&Rank](const Use &U) {
      auto It = Rank.find(&U);
      return It == Rank.end() ? ~0U : It->second;
    };
    V->sortUseList([&RankOf](const Use &L, const Use &R) {
      return RankOf(L) < RankOf(R);
    });
  };

  for (const auto &[I, Uses] : Order.Locals)
    Sort(Values.at(I), Uses);

  Module &M = *F.getParent();
  for (const auto &[Name, Uses] : Order.Globals) {
    GlobalValue *Global = M.getNamedValue(Name);
    revng_assert(Global != nullptr);
    Sort(Global, Uses);
  }
}

static void verify(const Module &M) {
  if (VerifyMoved and verifyModule(M, &dbgs()))
    revng_abort("A moved function's module does not verify");
}

MovedFunction moveToOwnModule(Function &F, ArrayRef<GlobalValue *> Extra) {
  Module &Source = *F.getParent();
  MovedFunction Result;
  Result.Order = recordUseListOrder(F);
  Result.Module = std::make_unique<Module>(F.getName(), F.getContext());
  Module &New = *Result.Module;
  New.setDataLayout(Source.getDataLayout());
  New.setTargetTriple(Source.getTargetTriple());

  // Collect the globals F references through its operands, through the
  // constants built from them and through the initializers of the global
  // variables among them, in the order of their first reference
  SetVector<GlobalValue *> Globals;
  SmallPtrSet<const Constant *, 32> Visited;
  SmallVector<Constant *, 32> Worklist;
  auto Visit = [&](Value *V) {
    auto *First = dyn_cast<Constant>(V);
    if (First == nullptr or not Visited.insert(First).second)
      return;

    Worklist.push_back(First);
    while (not Worklist.empty()) {
      Constant *C = Worklist.pop_back_val();
      if (auto *Global = dyn_cast<GlobalValue>(C)) {
        if (Global == &F)
          continue;
        Globals.insert(Global);
        auto *Variable = dyn_cast<GlobalVariable>(Global);
        if (Variable == nullptr or not Variable->hasInitializer())
          continue;
        C = Variable->getInitializer();
        if (Visited.insert(C).second)
          Worklist.push_back(C);
        continue;
      }

      // The blocks of another function cannot come along
      if (auto *Address = dyn_cast<BlockAddress>(C))
        revng_assert(Address->getFunction() == &F,
                     "A moved function takes the address of another "
                     "function's block");

      // Visit the operands in order, so that the first one is visited first
      for (Use &Operand : llvm::reverse(C->operands()))
        if (auto *Next = dyn_cast<Constant>(Operand.get()))
          if (Visited.insert(Next).second)
            Worklist.push_back(Next);
    }
  };
  for (Instruction &I : instructions(F))
    for (Use &Operand : I.operands())
      Visit(Operand.get());
  for (GlobalValue *Global : Extra)
    Visit(Global);

  // Recreate them in the new module
  ValueToValueMapTy Map;
  SmallVector<std::pair<unsigned, MDNode *>, 4> Metadata;
  for (GlobalValue *Global : Globals) {
    GlobalObject *Copy = nullptr;
    if (auto *Variable = dyn_cast<GlobalVariable>(Global)) {
      auto *NewVariable = new GlobalVariable(New,
                                             Variable->getValueType(),
                                             Variable->isConstant(),
                                             Variable->getLinkage(),
                                             nullptr,
                                             Variable->getName(),
                                             nullptr,
                                             Variable->getThreadLocalMode(),
                                             Variable->getAddressSpace());
      NewVariable->copyAttributesFrom(Variable);
      Copy = NewVariable;
    } else if (auto *Callee = dyn_cast<Function>(Global)) {
      // A declaration: the passes of a function read only the attributes of
      // its callees
      auto Linkage = Callee->hasExternalWeakLinkage() ?
                       GlobalValue::ExternalWeakLinkage :
                       GlobalValue::ExternalLinkage;
      auto *NewCallee = Function::Create(Callee->getFunctionType(),
                                         Linkage,
                                         Callee->getAddressSpace(),
                                         Callee->getName(),
                                         &New);
      NewCallee->setCallingConv(Callee->getCallingConv());
      NewCallee->setAttributes(Callee->getAttributes());
      Copy = NewCallee;
    } else {
      revng_abort("Only global variables and functions can be moved");
    }

    revng_assert(Copy->getName() == Global->getName());
    Metadata.clear();
    cast<GlobalObject>(Global)->getAllMetadata(Metadata);
    for (auto &[Kind, Node] : Metadata)
      if (Kind != LLVMContext::MD_dbg)
        Copy->setMetadata(Kind, Node);

    Map[Global] = Copy;
  }

  for (GlobalValue *Global : Globals) {
    auto *Variable = dyn_cast<GlobalVariable>(Global);
    if (Variable != nullptr and Variable->hasInitializer()) {
      auto *Initializer = MapValue(Variable->getInitializer(), Map);
      cast<GlobalVariable>(Map[Variable])->setInitializer(Initializer);
    }
  }

  // Move F and rebind it to the new globals. F keeps its instructions, so
  // the use lists of its own values stay as they are
  New.getFunctionList().splice(New.end(),
                               Source.getFunctionList(),
                               F.getIterator());
  for (Instruction &I : instructions(F))
    RemapInstruction(&I, Map, RF_IgnoreMissingLocals | RF_NoModuleLevelChanges);

  verify(New);
  return Result;
}

ShippedFunction shipFunction(Function &F, ArrayRef<GlobalValue *> Extra) {
  ShippedFunction Result;
  Result.Name = F.getName().str();

  MovedFunction Moved = moveToOwnModule(F, Extra);
  Result.Order = std::move(Moved.Order);
  raw_svector_ostream Stream(Result.Bitcode);
  WriteBitcodeToFile(*Moved.Module, Stream);

  // Constants built on the new globals live in the context: destroy them
  // with the module rather than leave them pointing to deleted globals
  Moved.Module->dropAllReferences();
  for (GlobalValue &Global : Moved.Module->global_values())
    Global.removeDeadConstantUsers();
  Moved.Module.reset();

  return Result;
}

std::unique_ptr<Module> receiveFunction(const ShippedFunction &Shipped,
                                        LLVMContext &Context) {
  StringRef Bitcode(Shipped.Bitcode.data(), Shipped.Bitcode.size());
  auto MaybeModule = parseBitcodeFile(MemoryBufferRef(Bitcode, Shipped.Name),
                                      Context);
  if (not MaybeModule)
    revng_abort(toString(MaybeModule.takeError()).c_str());

  std::unique_ptr<Module> Result = std::move(*MaybeModule);
  Function *F = Result->getFunction(Shipped.Name);
  revng_assert(F != nullptr);
  applyUseListOrder(*F, Shipped.Order);
  verify(*Result);
  return Result;
}
