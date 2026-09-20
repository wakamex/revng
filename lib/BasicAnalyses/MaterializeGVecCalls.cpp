//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "revng/Support/ResourceFinder.h"

using namespace llvm;

namespace {

/// Generic QEMU vector helpers use void pointers into CPU state. These do not
/// acquire the typed CPU-pointer annotations used by fix-helpers. Expose their
/// effects before ABI analysis by materializing the pointed-to register bytes.
/// Keep one backing allocation so overlapping source/destination ranges alias.
class MaterializeGVecCalls : public ModulePass {
public:
  static char ID;

  MaterializeGVecCalls() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    SmallVector<CallInst *> Calls;
    for (Function &F : M)
      for (Instruction &I : instructions(F))
        if (auto *Call = dyn_cast<CallInst>(&I))
          if (auto *Callee = Call->getCalledFunction())
            if (Callee->getName().starts_with("helper_gvec_"))
              Calls.push_back(Call);

    if (Calls.empty())
      return false;
    if (M.getDataLayout().isBigEndian())
      report_fatal_error("gvec CPU-state materialization needs little-endian "
                         "storage");

    // Use the shipped QEMU implementation, specialized to the call's constant
    // descriptor. Do not recreate each vector operation in the lifter.
    std::unique_ptr<Module> Helpers;
    if (llvm::any_of(Calls, [](CallInst *C) {
          return C->getCalledFunction()->isDeclaration();
        })) {
      auto *Arch = M.getNamedMetadata("revng.qemu_architecture");
      if (!Arch || Arch->getNumOperands() != 1)
        report_fatal_error("missing gvec helper architecture");
      auto Name = cast<MDString>(Arch->getOperand(0)->getOperand(0))
                    ->getString();
      auto File = revng::ResourceFinder.findFile(("/share/revng/"
                                                  "libtcg-helpers-full-"
                                                  + Name + ".bc")
                                                   .str());
      if (!File)
        report_fatal_error("cannot find gvec helper bitcode");
      SMDiagnostic Diagnostic;
      Helpers = parseIRFile(*File, Diagnostic, M.getContext());
      if (!Helpers)
        report_fatal_error("cannot read gvec helper bitcode");
    } else {
      Helpers = CloneModule(M);
    }

    std::map<uint64_t, GlobalVariable *> Registers;
    ValueToValueMapTy ExportMap;
    for (GlobalVariable &GV : M.globals()) {
      if (auto *MD = GV.getMetadata("revng.csv.offset")) {
        auto *Offset = mdconst::extract<ConstantInt>(MD->getOperand(0));
        auto *Copy = new GlobalVariable(*Helpers,
                                        GV.getValueType(),
                                        false,
                                        GlobalValue::ExternalLinkage,
                                        nullptr,
                                        "materialized_csv");
        Registers.emplace(Offset->getZExtValue(), Copy);
        ExportMap[Copy] = &GV;
      }
    }

    auto Locate = [&](uint64_t Offset) {
      auto It = Registers.upper_bound(Offset);
      if (It == Registers.begin())
        report_fatal_error("gvec pointer has no CPU-state register");
      --It;
      auto *Type = dyn_cast<IntegerType>(It->second->getValueType());
      uint64_t Byte = Offset - It->first;
      if (!Type || Type->getBitWidth() % 8 || Byte >= Type->getBitWidth() / 8)
        report_fatal_error("gvec access crosses unmapped CPU-state bytes");
      return std::pair(It->second, Byte);
    };

    for (CallInst *Original : Calls) {
      // Optimize temporary storage outside root: its CodeExtractor forbids
      // allocas, and running general CFG simplification there destroys markers.
      SmallVector<Type *> Types;
      SmallVector<Value *> Arguments;
      for (Value *Argument : Original->args()) {
        if (!isa<Constant>(Argument)) {
          Types.push_back(Argument->getType());
          Arguments.push_back(Argument);
        }
      }
      auto *FT = FunctionType::get(Type::getVoidTy(M.getContext()),
                                   Types,
                                   false);
      auto *Wrapper = Function::Create(FT,
                                       GlobalValue::InternalLinkage,
                                       "materialized_gvec",
                                       *Helpers);
      auto *Block = BasicBlock::Create(M.getContext(), "entry", Wrapper);
      auto *Call = cast<CallInst>(Original->clone());
      auto *Helper = Helpers
                       ->getFunction(Original->getCalledFunction()->getName());
      if (!Helper || Helper->getFunctionType() != Original->getFunctionType()
          || !Helper->getReturnType()->isVoidTy())
        report_fatal_error("missing gvec helper body");
      if (auto Error = Helper->materialize())
        report_fatal_error("cannot materialize gvec helper body");
      Call->setCalledFunction(Helper);
      unsigned Next = 0;
      for (unsigned I = 0; I < Call->arg_size(); ++I)
        if (!isa<Constant>(Call->getArgOperand(I)))
          Call->setArgOperand(I, Wrapper->getArg(Next++));
      Call->insertBefore(ReturnInst::Create(M.getContext(), Block));
      // The generic gvec ABI has a destination, sources/scalars, and a final
      // simd_desc. Additional writable pointers require a separate contract.
      unsigned Count = Call->arg_size();
      if (Count < 2)
        report_fatal_error("unsupported gvec helper signature");
      auto *Descriptor = dyn_cast<ConstantInt>(Call->getArgOperand(Count - 1));
      if (!Call->getArgOperand(0)->getType()->isPointerTy() || !Descriptor
          || Descriptor->getBitWidth() != 32)
        report_fatal_error("unsupported gvec helper signature or descriptor");
      uint64_t Desc = Descriptor->getZExtValue();
      uint64_t MaxSize = ((Desc & 255) + 1) * 8;
      uint64_t EncodedSize = (Desc >> 8) & 3;
      uint64_t OpSize = EncodedSize == 2 ? MaxSize : (EncodedSize + 1) * 8;
      if (OpSize > MaxSize)
        report_fatal_error("invalid gvec operation size");

      SmallVector<std::pair<unsigned, uint64_t>> Pointers;
      uint64_t Start = UINT64_MAX, End = 0;
      for (unsigned I = 0; I < Count; ++I) {
        Value *Argument = Call->getArgOperand(I);
        if (!Argument->getType()->isPointerTy())
          continue;
        if (I && !Helper->hasParamAttribute(I, Attribute::ReadOnly))
          report_fatal_error("gvec source is not read-only");
        auto *Address = dyn_cast<ConstantExpr>(Argument);
        if (!Address || Address->getOpcode() != Instruction::IntToPtr)
          report_fatal_error("gvec pointer is not a constant CPU-state offset");
        auto *Offset = dyn_cast<ConstantInt>(Address->getOperand(0));
        if (!Offset)
          report_fatal_error("gvec pointer has a nonconstant offset");
        uint64_t Base = Offset->getZExtValue();
        uint64_t Size = I == 0 ? MaxSize : OpSize;
        if (Base > UINT64_MAX - Size)
          report_fatal_error("gvec CPU-state range overflows");
        for (uint64_t Byte = 0; Byte < Size; ++Byte)
          Locate(Base + Byte);
        Start = std::min(Start, Base);
        End = std::max(End, Base + Size);
        Pointers.emplace_back(I, Base);
      }

      IRBuilder<> Before(Call);
      auto *Memory = Before.CreateAlloca(ArrayType::get(Before.getInt8Ty(),
                                                        End - Start));
      Memory->setAlignment(Align(16));
      auto Pointer = [&](IRBuilder<> &B, uint64_t Offset) {
        return B.CreateInBoundsGEP(B.getInt8Ty(),
                                   Memory,
                                   B.getInt64(Offset - Start));
      };
      for (auto [Index, Base] : Pointers) {
        if (Index || !Helper->hasParamAttribute(0, Attribute::WriteOnly)) {
          for (uint64_t Byte = 0; Byte < OpSize; ++Byte) {
            auto [CSV, Part] = Locate(Base + Byte);
            auto *Value = Before.CreateLoad(CSV->getValueType(), CSV);
            auto *Shifted = Before.CreateLShr(Value, Part * 8);
            Before.CreateStore(Before.CreateTrunc(Shifted, Before.getInt8Ty()),
                               Pointer(Before, Base + Byte));
          }
        }
        Call->setArgOperand(Index, Pointer(Before, Base));
      }

      IRBuilder<> After(Call->getNextNode());
      uint64_t Destination = Pointers.front().second;
      for (uint64_t Byte = 0; Byte < MaxSize; ++Byte) {
        auto [CSV, Part] = Locate(Destination + Byte);
        auto *Type = cast<IntegerType>(CSV->getValueType());
        auto *Value = After.CreateLoad(After.getInt8Ty(),
                                       Pointer(After, Destination + Byte));
        auto *Extended = After.CreateZExt(Value, Type);
        auto *Shifted = After.CreateShl(Extended, Part * 8);
        APInt Mask = APInt::getBitsSet(Type->getBitWidth(),
                                       Part * 8,
                                       Part * 8 + 8);
        auto *Old = After.CreateLoad(Type, CSV);
        auto *Kept = After.CreateAnd(Old, ConstantInt::get(Type, ~Mask));
        After.CreateStore(After.CreateOr(Kept, Shifted), CSV);
      }

      for (unsigned Round = 0;; ++Round) {
        SmallVector<CallInst *> Inline;
        for (Instruction &I : instructions(Wrapper))
          if (auto *CI = dyn_cast<CallInst>(&I))
            if (auto *F = CI->getCalledFunction()) {
              if (auto Error = F->materialize())
                report_fatal_error("cannot materialize gvec dependency");
              if (!F->isDeclaration() && !F->isIntrinsic())
                Inline.push_back(CI);
            }
        if (Inline.empty())
          break;
        if (Round == 16)
          report_fatal_error("gvec helper has recursive or excessive call "
                             "depth");
        for (CallInst *CI : Inline) {
          InlineFunctionInfo Info;
          if (!InlineFunction(*CI, Info, false, nullptr, false).isSuccess())
            report_fatal_error("cannot inline gvec helper");
        }
      }
      legacy::FunctionPassManager PM(Helpers.get());
      PM.add(createSROAPass());
      PM.add(createInstructionCombiningPass());
      PM.add(createCFGSimplificationPass());
      PM.add(createLoopSimplifyPass());
      PM.add(createLCSSAPass());
      PM.add(createLoopUnrollPass(3, false, false, 1000000, -1, 0, 0, 1, 0));
      PM.add(createSROAPass());
      PM.add(createInstructionCombiningPass());
      PM.add(createCFGSimplificationPass());
      PM.doInitialization();
      PM.run(*Wrapper);
      PM.doFinalization();
      stripDebugInfo(*Wrapper);
      for (Instruction &I : instructions(Wrapper)) {
        if (isa<AllocaInst>(I))
          report_fatal_error("gvec temporary storage did not scalarize");
        if (isa<CallBase>(I))
          report_fatal_error("gvec specialization retains a call");
        for (Value *Operand : I.operands())
          if (auto *GV = dyn_cast<GlobalValue>(Operand->stripPointerCasts()))
            if (!ExportMap.count(GV))
              report_fatal_error("gvec specialization retains unowned global "
                                 "state");
      }
      auto *Scalarized = Function::Create(FT,
                                          GlobalValue::InternalLinkage,
                                          "scalarized_gvec",
                                          M);
      for (unsigned I = 0; I < Wrapper->arg_size(); ++I)
        ExportMap[Wrapper->getArg(I)] = Scalarized->getArg(I);
      SmallVector<ReturnInst *> Returns;
      CloneFunctionInto(Scalarized,
                        Wrapper,
                        ExportMap,
                        CloneFunctionChangeType::DifferentModule,
                        Returns);
      IRBuilder<> Replacement(Original);
      auto *ReplacementCall = Replacement.CreateCall(Scalarized, Arguments);
      InlineFunctionInfo Info;
      if (!InlineFunction(*ReplacementCall, Info, false, nullptr, false)
             .isSuccess())
        report_fatal_error("cannot inline scalarized gvec helper");
      Original->eraseFromParent();
      Scalarized->eraseFromParent();
      Wrapper->eraseFromParent();
    }
    return true;
  }
};

char MaterializeGVecCalls::ID = 0;
RegisterPass<MaterializeGVecCalls> X("materialize-gvec-calls",
                                     "Expose generic vector helper CPU-state "
                                     "effects",
                                     false,
                                     false);

} // namespace
