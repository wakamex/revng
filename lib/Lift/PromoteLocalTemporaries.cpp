//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "revng/Support/Assert.h"

#include "PromoteLocalTemporaries.h"

using namespace llvm;

namespace {

using ReachingStores = DenseMap<AllocaInst *, Value *>;

/// Visit every block once, each after its single predecessor when it has one,
/// passing the stores that reach its start along that chain of predecessors.
template<typename Visitor>
void visitPredecessorChains(Function &F, Visitor &&Visit) {
  DenseSet<BasicBlock *> Visited;
  std::vector<std::pair<BasicBlock *, ReachingStores>> Stack;

  auto Walk = [&](BasicBlock *Root) {
    Visited.insert(Root);
    Stack.emplace_back(Root, ReachingStores());
    while (not Stack.empty()) {
      auto [BB, State] = std::move(Stack.back());
      Stack.pop_back();
      Visit(*BB, State);

      SmallVector<BasicBlock *, 4> Children;
      for (BasicBlock *Successor : successors(BB))
        if (Successor != BB and Successor->getSinglePredecessor() == BB
            and Visited.insert(Successor).second)
          Children.push_back(Successor);

      for (size_t I = 0; I < Children.size(); ++I) {
        if (I + 1 == Children.size())
          Stack.emplace_back(Children[I], std::move(State));
        else
          Stack.emplace_back(Children[I], State);
      }
    }
  };

  for (BasicBlock &BB : F) {
    BasicBlock *Predecessor = BB.getSinglePredecessor();
    if (not Visited.contains(&BB)
        and (Predecessor == nullptr or Predecessor == &BB))
      Walk(&BB);
  }

  // Blocks on cycles of single predecessors have no chain start; visit them
  // with nothing reaching them.
  for (BasicBlock &BB : F)
    if (not Visited.contains(&BB))
      Walk(&BB);
}

/// Allocas described by debug information are left to PromoteMemToReg, which
/// also rewrites their debug records.
bool isEntryAlloca(Value *V, BasicBlock &Entry) {
  auto *Alloca = dyn_cast<AllocaInst>(V);
  return Alloca != nullptr and Alloca->getParent() == &Entry
         and not Alloca->isArrayAllocation()
         and not Alloca->isUsedByMetadata();
}

} // namespace

bool promoteLocalTemporaries(Function &F, const DominatorTree &DT) {
  if (F.empty())
    return false;
  BasicBlock &Entry = F.getEntryBlock();

  // Find the entry allocas that are only loaded and stored as a whole, and
  // whose every load follows a store along its chain of single predecessors.
  // PromoteMemToReg treats loads in unreachable blocks differently depending on
  // the alloca, so allocas used there are left to it.
  DenseSet<AllocaInst *> Rejected;
  visitPredecessorChains(F, [&](BasicBlock &BB, ReachingStores &State) {
    bool Reachable = DT.isReachableFromEntry(&BB);
    for (Instruction &I : BB) {
      for (Use &Operand : I.operands()) {
        if (not isEntryAlloca(Operand.get(), Entry))
          continue;
        auto *Alloca = cast<AllocaInst>(Operand.get());
        if (not Reachable) {
          Rejected.insert(Alloca);
          continue;
        }
        Type *Allocated = Alloca->getAllocatedType();
        if (auto *Load = dyn_cast<LoadInst>(&I);
            Load != nullptr and Load->isSimple()
            and Load->getType() == Allocated) {
          if (State.count(Alloca) == 0)
            Rejected.insert(Alloca);
        } else if (auto *Store = dyn_cast<StoreInst>(&I);
                   Store != nullptr and Store->isSimple()
                   and Operand.getOperandNo()
                         == StoreInst::getPointerOperandIndex()
                   and Store->getValueOperand()->getType() == Allocated) {
          State[Alloca] = Store->getValueOperand();
        } else {
          Rejected.insert(Alloca);
        }
      }
    }
  });

  // Replace each load of an accepted alloca with the value stored last before
  // it, as PromoteMemToReg does, and drop the stores.
  bool Changed = false;
  visitPredecessorChains(F, [&](BasicBlock &BB, ReachingStores &State) {
    if (not DT.isReachableFromEntry(&BB))
      return;
    for (Instruction &I : make_early_inc_range(BB)) {
      if (auto *Load = dyn_cast<LoadInst>(&I)) {
        Value *Pointer = Load->getPointerOperand();
        if (isEntryAlloca(Pointer, Entry)
            and not Rejected.contains(cast<AllocaInst>(Pointer))) {
          Value *Stored = State.lookup(cast<AllocaInst>(Pointer));
          revng_assert(Stored != nullptr);
          Load->replaceAllUsesWith(Stored);
          Load->eraseFromParent();
          Changed = true;
        }
      } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
        Value *Pointer = Store->getPointerOperand();
        if (isEntryAlloca(Pointer, Entry)
            and not Rejected.contains(cast<AllocaInst>(Pointer))) {
          State[cast<AllocaInst>(Pointer)] = Store->getValueOperand();
          Store->eraseFromParent();
          Changed = true;
        }
      }
    }
  });

  for (Instruction &I : make_early_inc_range(Entry)) {
    auto *Alloca = dyn_cast<AllocaInst>(&I);
    if (Alloca != nullptr and isEntryAlloca(Alloca, Entry)
        and not Rejected.contains(Alloca)) {
      revng_assert(Alloca->use_empty());
      Alloca->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}
