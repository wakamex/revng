/// Promote CSVs in form of global variables to local variables.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/STLExtras.h"

#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/OpaqueRegisterUser.h"

using namespace llvm;

llvm::PreservedAnalyses
PromoteGlobalToLocalPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) {

  // Collect the CSVs used by the current function.
  std::map<GlobalVariable *, Value *> CSVMap;
  for (auto &BB : F) {
    for (auto &I : BB) {
      Value *Pointer = nullptr;
      if (auto *Load = dyn_cast<LoadInst>(&I))
        Pointer = skipCasts(Load->getPointerOperand());
      else if (auto *Store = dyn_cast<StoreInst>(&I))
        Pointer = skipCasts(Store->getPointerOperand());
      else
        continue;

      if (auto *CSV = dyn_cast_or_null<GlobalVariable>(Pointer))
        if (not ShouldPromote or ShouldPromote(*CSV))
          CSVMap.try_emplace(CSV);
    }
  }

  // Collect every use of those CSVs in this function, directly or through a
  // cast constant expression, in a single walk of the function. Replacing the
  // uses one CSV at a time with `replaceAllUsesInFunctionWith` walks each
  // CSV's uses in the whole module, `root` and every other function included,
  // which makes the pass cost grow with the module rather than the function.
  std::map<GlobalVariable *, SmallVector<Use *, 16>> DirectUses;
  std::map<GlobalVariable *, SmallVector<Use *, 4>> CastUses;
  for (auto &BB : F) {
    for (auto &I : BB) {
      for (Use &U : I.operands()) {
        if (auto *CSV = dyn_cast<GlobalVariable>(U.get())) {
          if (CSVMap.contains(CSV))
            DirectUses[CSV].push_back(&U);
        } else if (auto *CE = dyn_cast<ConstantExpr>(U.get())) {
          auto *CSV = dyn_cast<GlobalVariable>(CE->getOperand(0));
          if (CE->isCast() and CSV != nullptr and CSVMap.contains(CSV))
            CastUses[CSV].push_back(&U);
        }
      }
    }
  }

  revng::IRBuilder Builder(&F.getEntryBlock().front());

  // Create an equivalent local variable, replace all the uses of the CSV.
  for (GlobalVariable *CSV : toSortedByName(llvm::make_first_range(CSVMap))) {
    auto *CSVTy = CSV->getValueType();
    auto *Alloca = Builder.CreateAlloca(CSVTy, nullptr, CSV->getName());

    for (Use *U : DirectUses[CSV])
      U->set(Alloca);

    // A cast of the CSV may be used elsewhere too: give this use its own
    // instruction casting the local variable
    for (Use *U : CastUses[CSV]) {
      auto *CE = cast<ConstantExpr>(U->get());
      Instruction *Cast = CE->getAsInstruction();
      Cast->replaceUsesOfWith(CSV, Alloca);
      Cast->insertBefore(cast<Instruction>(U->getUser()));
      U->set(Cast);
    }

    CSVMap[CSV] = Alloca;
  }

  // Load all the CSVs and store their value onto the local variables, in the
  // order of their names: the map's order is the order of their addresses.
  for (GlobalVariable *CSV : toSortedByName(llvm::make_first_range(CSVMap)))
    Builder.CreateStore(Builder.createLoad(CSV), CSVMap.at(CSV));

  return PreservedAnalyses::none();
}
