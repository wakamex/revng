//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include "revng/ABI/ModelHelpers.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Model/Binary.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/Segment.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

#include "../FuncOrCallInst.h"
#include "DLATypeSystemBuilder.h"

using namespace dla;
using namespace llvm;

using TSBuilder = DLATypeSystemLLVMBuilder;

void TSBuilder::initializeSegments(SegmentNodeMapT &Segments) {
  for (const model::Segment &S : Model.Segments()) {
    // Initialize a node for every segment
    LayoutTypeSystemNode
      *SegmentNode = Segments[&S] = TS.createArtificialLayoutType();
    // Set the Size, which is known for segments.
    SegmentNode->Size = S.VirtualSize();
    // Set NonScalar to true, so that it cannot be removed from
    // the optimization steps of DLA's middle-end
    SegmentNode->NonScalar = true;
  }
}

/// Return the model type for \p F.
/// \p F must be either isolated or dynamic.
static const model::TypeDefinition *
getFunctionPrototype(const model::Binary &Model, const Function &F) {
  if (FunctionTags::Isolated.isTagOf(&F)) {
    const model::Function *ModelFunction = llvmToModelFunction(Model, F);
    revng_assert(ModelFunction);
    return Model.prototypeOrDefault(ModelFunction->prototype());
  }

  revng_assert(FunctionTags::DynamicFunction.isTagOf(&F));
  llvm::StringRef SymbolName = F.getName().drop_front(strlen("dynamic_"));

  auto It = Model.ImportedDynamicFunctions().find(SymbolName.str());
  revng_assert(It != Model.ImportedDynamicFunctions().end());
  return Model.prototypeOrDefault(It->prototype());
}

bool TSBuilder::createInterproceduralTypes(llvm::Module &M,
                                           const SegmentNodeMapT
                                             &SegmentNodeMap) {
  for (const Function &F : M.functions()) {

    auto FTags = FunctionTags::TagsSet::from(&F);
    // Skip intrinsics
    if (F.isIntrinsic())
      continue;

    // Ignore everything that is not isolated or dynamic
    if (not FunctionTags::Isolated.isTagOf(&F)
        and not FunctionTags::DynamicFunction.isTagOf(&F))
      continue;

    revng_assert(not F.isVarArg());

    const model::TypeDefinition *Prototype = getFunctionPrototype(Model, F);
    revng_assert(Prototype);

    const auto *RFT = dyn_cast<model::RawFunctionDefinition>(Prototype);
    const auto *CABIFT = dyn_cast<model::CABIFunctionDefinition>(Prototype);
    if (RFT) {
      revng_assert(F.arg_size() == RFT->Arguments().size()
                   or (not RFT->StackArgumentsType().isEmpty()
                       and (F.arg_size() == RFT->Arguments().size() + 1)));
    } else if (CABIFT) {
      revng_assert(CABIFT->Arguments().size() == F.arg_size());
    } else {
      revng_abort();
    }

    // Create the Function's return types
    auto FRetTypes = getOrCreateLayoutTypes(F);
    for (auto [ReturnNode, Created] : FRetTypes)
      ReturnNode->IsTypeRoot = true;

    // Unify declarations of the same function, including copies in callers.
    const auto [It,
                NewFunction] = VisitedFunctions.insert({ { Prototype,
                                                           F.getName().str() },
                                                         &F });

    // The canonical declaration supplies the shared argument and return nodes.
    FuncOrCallInst SameFunction;
    if (not NewFunction) {
      SameFunction = It->second;
      revng_assert(not SameFunction.isNull());
      revng_assert(F.arg_size() == SameFunction.arg_size());

      auto OtherRetVals = getLayoutTypes(*SameFunction.getVal());
      revng_assert(FRetTypes.size() == OtherRetVals.size());
      for (auto &&[N1, N2] : llvm::zip(OtherRetVals, FRetTypes))
        TS.addEqualityLink(N1, N2.first);
    }

    // Create types for the Function's arguments
    for (const auto &ArgVal : F.args()) {
      auto ArgIndex = ArgVal.getArgNo();
      // Arguments can only be integers and pointers
      revng_assert(isa<IntegerType>(ArgVal.getType())
                   or isa<PointerType>(ArgVal.getType()));
      auto &&[ArgNode, _] = getOrCreateLayoutType(&ArgVal);
      revng_assert(ArgNode);
      ArgNode->IsTypeRoot = true;

      model::UpcastableType ArgumentModelType;
      if (RFT) {
        const auto &ModelArgs = RFT->Arguments();
        if (ArgIndex < ModelArgs.size())
          ArgumentModelType = std::next(ModelArgs.begin(), ArgIndex)->Type();
        else
          ArgumentModelType = RFT->StackArgumentsType();
      } else {
        revng_assert(CABIFT);
        ArgumentModelType = CABIFT->Arguments().at(ArgIndex).Type();
      }

      if (model::PointerType *Pointer = ArgumentModelType->getPointer()) {
        if (const model::UpcastableType &Pointee = Pointer->PointeeType()) {
          if (not Pointee->isVoidPrimitive()) {
            bool IsFunction = Pointee->isPrototype();
            bool IsScalar = !IsFunction && Pointee->isScalar();

            auto MaybeSize = Pointee->size();
            bool IsSized = MaybeSize.has_value();

            // If it is not scalar then it must be sized or a function type
            revng_assert(IsFunction or IsScalar or IsSized);

            if (not IsScalar) {
              ArgNode->NonScalar = true;
              if (IsSized)
                ArgNode->Size = *MaybeSize;
              else if (IsFunction)
                ArgNode->Size = getPointerSize(Model.Architecture());
            } else {
              // Skip char, because they alias and propagate weird information.
              if (IsSized and *MaybeSize > 1)
                ArgNode->Size = *MaybeSize;
              else if (IsFunction)
                ArgNode->Size = getPointerSize(Model.Architecture());
            }
          }
        }
      }

      // Connect arguments of declarations referring to the same function.
      if (not SameFunction.isNull()) {
        auto &OtherArg = *(SameFunction.getArg(ArgIndex));
        auto *OtherArgNode = getLayoutType(&OtherArg);
        revng_assert(OtherArgNode);
        TS.addEqualityLink(ArgNode, OtherArgNode);
      }
    }

    for (const BasicBlock &B : F) {
      for (const Instruction &I : B) {
        if (auto *Call = getCallToIsolatedFunction(&I)) {

          const Function *Callee = getCallee(Call);
          if (not Callee)
            continue;

          // Devirtualization can identify a callee without updating the call
          // site's prototype. Raw prototypes still identify arguments by their
          // machine registers, even when their LLVM positions differ.
          const auto *CallPrototype = getCallSitePrototype(Model, Call);
          const auto *CalleePrototype = getFunctionPrototype(Model, *Callee);
          const auto
            *CallRaw = dyn_cast<model::RawFunctionDefinition>(CallPrototype);
          const auto *
            CalleeRaw = dyn_cast<model::RawFunctionDefinition>(CalleePrototype);
          bool SamePrototype = CallPrototype == CalleePrototype;
          if (not SamePrototype
              and (not CallRaw or not CalleeRaw
                   or CallRaw->Architecture() != CalleeRaw->Architecture()))
            continue;

          for (auto &&[ArgNo, ArgUse] : llvm::enumerate(Call->args())) {
            unsigned FormalIndex = ArgNo;
            if (not SamePrototype) {
              // Stack arguments need a separate offset-based correspondence.
              if (ArgNo >= CallRaw->Arguments().size())
                continue;
              auto &Argument = *std::next(CallRaw->Arguments().begin(), ArgNo);
              auto It = CalleeRaw->Arguments().find(Argument.Location());
              if (It == CalleeRaw->Arguments().end())
                continue;
              FormalIndex = std::distance(CalleeRaw->Arguments().begin(), It);
            }
            revng_assert(FormalIndex < Callee->arg_size());
            // Create the layout for the call arguments
            const auto *ActualArg = ArgUse.get();
            revng_assert(isa<IntegerType>(ActualArg->getType())
                         or isa<PointerType>(ActualArg->getType()));

            // Avoid creating layouts for pointers to functions.
            if (isPointerToFunctionExpression(ActualArg))
              continue;

            auto ActualTypes = getOrCreateLayoutTypes(*ActualArg);

            // Create the layout for the formal arguments.
            Value *FormalArg = Callee->getArg(FormalIndex);
            if (ActualArg->getType() != FormalArg->getType())
              continue;
            revng_assert(isa<IntegerType>(FormalArg->getType())
                         or isa<PointerType>(FormalArg->getType()));
            auto FormalTypes = getOrCreateLayoutTypes(*FormalArg);
            revng_assert(1ULL == ActualTypes.size());
            revng_assert(1ULL == FormalTypes.size());

            auto FieldNum = FormalTypes.size();
            if (not isa<ConstantInt>(ActualArg)) {
              for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
                TS.addInstanceLink(ActualTypes[FieldId].first,
                                   FormalTypes[FieldId].first,
                                   OffsetExpression{});
                auto *Placeholder = TS.createArtificialLayoutType();
                Placeholder->Size = getPointerSize(Model.Architecture());
                TS.addPointerLink(Placeholder, ActualTypes[FieldId].first);
              }
            }
          }
        } else if (auto *PHI = dyn_cast<PHINode>(&I)) {
          revng_assert(isa<IntegerType>(PHI->getType())
                       or isa<PointerType>(PHI->getType())
                       or isa<StructType>(PHI->getType()));
          auto PHITypes = getOrCreateLayoutTypes(*PHI);
          for (const Use &Incoming : PHI->incoming_values()) {
            revng_assert(isa<IntegerType>(Incoming->getType())
                         or isa<PointerType>(Incoming->getType())
                         or isa<StructType>(Incoming->getType()));

            // Avoid creating layouts for pointers to functions.
            if (isPointerToFunctionExpression(Incoming))
              continue;

            auto InTypes = getOrCreateLayoutTypes(*Incoming.get());
            revng_assert(PHITypes.size() == InTypes.size());
            revng_assert((PHITypes.size() == 1ULL)
                         or isa<StructType>(PHI->getType()));
            auto FieldNum = PHITypes.size();
            if (not isa<ConstantInt>(Incoming)) {
              for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
                TS.addInstanceLink(InTypes[FieldId].first,
                                   PHITypes[FieldId].first,
                                   OffsetExpression{});
              }
            }
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          if (Value *RetVal = RetI->getReturnValue()) {
            // TODO: An array-of-bytes return value for a CABI function
            // returning an aggregate. In order to handle it properly we should
            // visit it recursively and have first-class support for model types
            // in DLA, which we currently don't have.
            // So, for now we just bail out, accepting to degrade the quality of
            // the results. We don't expect this to affect users much in
            // practice, because either the input has no debug symbols (and in
            // that case DLA sees mostly Raw functions, and this case doesn't
            // trigger) or the model (via debug symbols, or user input) has lots
            // of good information on CABI function types, in which case the
            // role of DLA is not that important interprocedurally.
            if (isArrayOfBytes(RetVal->getType()))
              continue;
            revng_assert(isa<StructType>(RetVal->getType())
                         or isa<IntegerType>(RetVal->getType())
                         or isa<PointerType>(RetVal->getType()));

            // Avoid creating layouts for pointers to functions.
            if (isPointerToFunctionExpression(RetVal))
              continue;

            auto RetTypes = getOrCreateLayoutTypes(*RetVal);
            revng_assert(RetTypes.size() == FRetTypes.size());
            auto FieldNum = RetTypes.size();
            if (not isa<ConstantInt>(RetVal)) {
              for (auto FieldId = 0ULL; FieldId < FieldNum; ++FieldId) {
                if (RetTypes[FieldId].first != nullptr) {
                  TS.addInstanceLink(RetTypes[FieldId].first,
                                     FRetTypes[FieldId].first,
                                     OffsetExpression{});
                  auto *Placeholder = TS.createArtificialLayoutType();
                  Placeholder->Size = getPointerSize(Model.Architecture());
                  TS.addPointerLink(Placeholder, RetTypes[FieldId].first);
                }
              }
            }
          }
        }
      }
    }
  }

  // Connect segment getters to the shared per-segment nodes. The nodes
  // themselves are created once by initializeSegments, so that getters coming
  // from different modules all converge on the same node for a given segment.

  const auto &Segments = Model.Segments();

  // Connect each segment's node to the DLA node of its global variable. After
  // inline-segment-global-getters, segment accesses reference this global
  // directly (see isAlwaysAddress in the base-address explorer), so the fields
  // recovered from them live on the global's node. We go through the segment
  // globals rather than the segment getters: the getters are unused after
  // inlining and are not guaranteed to survive until the DLA runs.
  for (llvm::GlobalVariable &SegmentGlobalVar :
       FunctionTags::SegmentGlobal.globals(&M)) {
    const model::Segment
      *Segment = &Segments.at(SegmentGlobal::getAddress(SegmentGlobalVar));
    LayoutTypeSystemNode *SegmentNode = SegmentNodeMap.at(Segment);

    LayoutTypeSystemNode
      *SegmentGlobalNode = getOrCreateLayoutType(&SegmentGlobalVar).first;
    TS.addEqualityLink(SegmentNode, SegmentGlobalNode);
  }

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyConsistency());
  return TS.getNumLayouts() != 0;
}
