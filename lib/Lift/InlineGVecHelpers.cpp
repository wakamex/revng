//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "revng/InlineHelpers/LinkHelpersToInline.h"
#include "revng/Lift/VariableManager.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"

#include "InlineGVecHelpers.h"

using namespace llvm;

static bool specialize(CallInst &Call, VariableManager &Variables) {
  if (not Call.getType()->isVoidTy()
      or not llvm::all_of(Call.args(),
                          [](Value *V) { return isa<Constant>(V); }))
    return false;

  Module &M = *Call.getModule();
  auto &Context = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Context);
  auto *FT = FunctionType::get(Type::getVoidTy(Context), { PtrTy }, false);
  auto *Wrapper = Function::Create(FT,
                                   GlobalValue::InternalLinkage,
                                   "specialized_gvec",
                                   M);
  auto Cleanup = make_scope_exit([&]() { Wrapper->eraseFromParent(); });
  revng::IRBuilder Builder(BasicBlock::Create(Context, "", Wrapper));
  Value *Memory = Wrapper->getArg(0);
  auto *IntPtrTy = Builder.getIntPtrTy(M.getDataLayout());
  SmallVector<Value *> Arguments;
  for (Value *Argument : Call.args()) {
    if (Argument->getType()->isPointerTy()) {
      auto *Integer = ConstantExpr::getPtrToInt(cast<Constant>(Argument),
                                                IntPtrTy);
      auto *Offset = dyn_cast<ConstantInt>(Integer);
      if (Offset == nullptr
          or Argument->getType()->getPointerAddressSpace() != 0)
        return false;
      Arguments.push_back(Builder.CreateGEP(Builder.getInt8Ty(),
                                            Memory,
                                            Offset));
    } else {
      Arguments.push_back(Argument);
    }
  }
  auto *HelperCall = Builder.CreateCall(Call.getCalledFunction(), Arguments);
  Builder.CreateRetVoid();
  InlineFunctionInfo IFI;
  if (not InlineFunction(*HelperCall, IFI, false, nullptr, false).isSuccess())
    return false;

  // Specialize only this helper. Changing root's CFG would invalidate its
  // instruction and call-boundary markers. A shared base preserves aliases.
  legacy::FunctionPassManager PM(&M);
  PM.add(createSROAPass());
  PM.add(createInstructionCombiningPass());
  PM.add(createCFGSimplificationPass());
  PM.add(createLoopSimplifyPass());
  PM.add(createLCSSAPass());
  PM.add(createLoopUnrollPass(3, false, false, 4096, -1, 0, 0, 1, 0));
  PM.add(createInstructionCombiningPass());
  PM.add(createCFGSimplificationPass());
  PM.doInitialization();
  PM.run(*Wrapper);
  PM.doFinalization();
  stripDebugInfo(*Wrapper);
  if (Wrapper->size() != 1 or Wrapper->getInstructionCount() > 4096
      or not isa<ReturnInst>(Wrapper->front().getTerminator()))
    return false;

  auto *CPU = StructType::getTypeByName(Context, "struct.ArchCPU");
  uint64_t CPUSize = M.getDataLayout().getTypeAllocSize(CPU);
  SmallVector<std::pair<Instruction *, uint64_t>> Accesses;
  for (Instruction &I : instructions(Wrapper)) {
    auto *Load = dyn_cast<LoadInst>(&I);
    auto *Store = dyn_cast<StoreInst>(&I);
    if (Load == nullptr and Store == nullptr) {
      if (isa<CallBase, AllocaInst>(I))
        return false;
      continue;
    }
    if ((Load != nullptr and not Load->isSimple())
        or (Store != nullptr and not Store->isSimple()))
      return false;
    Value *Pointer = Load != nullptr ? Load->getPointerOperand() :
                                       Store->getPointerOperand();
    auto *Type = dyn_cast<IntegerType>(Load != nullptr ?
                                         Load->getType() :
                                         Store->getValueOperand()->getType());
    int64_t Offset = 0;
    auto *Base = GetPointerBaseWithConstantOffset(Pointer,
                                                  Offset,
                                                  M.getDataLayout());
    if (Type == nullptr or Type->getBitWidth() % 8 != 0 or Base != Memory
        or Offset < 0 or uint64_t(Offset) >= CPUSize
        or Type->getBitWidth() / 8 > CPUSize - Offset)
      return false;
    for (unsigned Byte = 0; Byte < Type->getBitWidth() / 8; ++Byte) {
      auto *CSV = Variables.getByCPUStateOffsetWithRemainder(Offset + Byte)
                    .first;
      if (CSV == nullptr or CSV->getValueType()->getIntegerBitWidth() > 64)
        return false;
    }
    Accesses.emplace_back(&I, Offset);
  }

  // Use the same register access routines as the lifter. Splitting at bytes
  // also handles accesses that cross fields, including partial overlap.
  for (auto [I, Offset] : Accesses) {
    Builder.SetInsertPoint(I);
    auto *Load = dyn_cast<LoadInst>(I);
    auto *Store = dyn_cast<StoreInst>(I);
    auto *Type = cast<IntegerType>(Load != nullptr ?
                                     Load->getType() :
                                     Store->getValueOperand()->getType());
    Value *Result = ConstantInt::get(Type, 0);
    unsigned Size = Type->getBitWidth() / 8;
    for (unsigned Byte = 0; Byte < Size; ++Byte) {
      unsigned Index = M.getDataLayout().isLittleEndian() ? Byte :
                                                            Size - Byte - 1;
      unsigned Shift = 8 * Index;
      if (Load != nullptr) {
        Value *Value = Variables.loadFromCPUStateOffset(Builder,
                                                        1,
                                                        Offset + Byte);
        Value = Builder.CreateZExtOrTrunc(Value, Type);
        Result = Builder.CreateOr(Result, Builder.CreateShl(Value, Shift));
      } else {
        Value *Value = Builder.CreateLShr(Store->getValueOperand(), Shift);
        Value = Builder.CreateTrunc(Value, Builder.getInt8Ty());
        auto Written = Variables.storeToCPUStateOffset(Builder,
                                                       1,
                                                       Offset + Byte,
                                                       Value);
        revng_assert(Written.has_value());
      }
    }
    if (Load != nullptr)
      Load->replaceAllUsesWith(Result);
    I->eraseFromParent();
  }

  legacy::FunctionPassManager Finish(&M);
  Finish.add(createEarlyCSEPass(true));
  Finish.add(createInstructionCombiningPass());
  Finish.add(createDeadStoreEliminationPass());
  Finish.add(createInstructionCombiningPass());
  Finish.doInitialization();
  Finish.run(*Wrapper);
  Finish.doFinalization();
  if (not Memory->use_empty())
    return false;

  Builder.SetInsertPoint(&Call);
  auto *Replacement = Builder.CreateCall(Wrapper, { PoisonValue::get(PtrTy) });
  InlineFunctionInfo FinalIFI;
  auto Inlined = InlineFunction(*Replacement, FinalIFI, false, nullptr, false);
  revng_assert(Inlined.isSuccess());
  Call.eraseFromParent();
  return true;
}

void inlineGVecHelpers(Function &Root, VariableManager &Variables) {
  SmallVector<CallInst *> Calls;
  for (Instruction &I : instructions(Root))
    if (auto *Call = dyn_cast<CallInst>(&I))
      if (auto *Helper = Call->getCalledFunction())
        if (Helper->getName().starts_with("helper_gvec_")
            and isInlineHelper(*Helper))
          Calls.push_back(Call);
  if (Calls.empty())
    return;

  LinkHelpersToInlinePass Link;
  Link.runOnModule(*Root.getParent());
  for (CallInst *Call : Calls)
    if (not specialize(*Call, Variables))
      // Keep unsupported calls opaque, including during later inlining.
      Call->getCalledFunction()->setSection("");
}
