#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/FunctionSummaryOracle.h"
#include "revng/EarlyFunctionAnalysis/TemporaryOpaqueFunction.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/NamedTypedRegister.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/UniqueValuePtr.h"

namespace efa {

class CallHandler;
class OutlinedFunctionsMap;

/// An outlined function helper object.
struct OutlinedFunction {
public:
  MetaAddress Address;

  /// The marker for indirect jumps
  // NOTE: do not move this field, F holds a reference to it and therefore it
  //       needs to be destroyed first.
  UniqueValuePtr<llvm::Function> IndirectBranchInfoMarker;

  /// The actual LLVM outlined function
  UniqueValuePtr<llvm::Function> Function;

  // Special blocks
  llvm::BasicBlock *AnyPCCloned = nullptr;
  llvm::BasicBlock *UnexpectedPCCloned = nullptr;

  std::vector<MetaAddress> InlinedFunctionsByIndex = { MetaAddress::invalid() };

public:
  OutlinedFunction() = default;

  OutlinedFunction(const OutlinedFunction &Other) = delete;
  OutlinedFunction &operator=(const OutlinedFunction &) = delete;

  OutlinedFunction(OutlinedFunction &&Other) { *this = std::move(Other); }

  OutlinedFunction &operator=(OutlinedFunction &&Other) {
    if (this != &Other) {
      Address = Other.Address;
      Function = std::move(Other.Function);
      IndirectBranchInfoMarker = std::move(Other.IndirectBranchInfoMarker);
      AnyPCCloned = Other.AnyPCCloned;
      UnexpectedPCCloned = Other.UnexpectedPCCloned;
      InlinedFunctionsByIndex = Other.InlinedFunctionsByIndex;

      Other.Address = MetaAddress::invalid();
      Other.Function = nullptr;
      Other.IndirectBranchInfoMarker.reset();
      Other.AnyPCCloned = nullptr;
      Other.UnexpectedPCCloned = nullptr;
    }
    return *this;
  }

public:
  llvm::Function *releaseFunction() {
    auto *ToReturn = Function.release();
    Function = nullptr;
    return ToReturn;
  }
};

/// This class, given an Oracle, can outline functions from root
class Outliner {
private:
  llvm::Module &M;
  RootFunction &RootF;
  const CSVGlobals &Globals;
  FunctionSummaryOracle &Oracle;

  /// UnexpectedPCMarker is used to indicate that `unexpectedpc` basic
  /// block of a function to inline need to be adjusted to jump to
  /// `unexpectedpc` of their caller.
  TemporaryOpaqueFunction UnexpectedPCMarker;

  OpaqueFunctionsPool<llvm::Type *> OpaqueReturnAddress;

  std::optional<llvm::CodeExtractorAnalysisCache> OwnCEAC;
  const llvm::CodeExtractorAnalysisCache &CEAC;

public:
  Outliner(llvm::Module &M,
           RootFunction &RootF,
           const CSVGlobals &Globals,
           FunctionSummaryOracle &Oracle) :
    M(M),
    RootF(RootF),
    Globals(Globals),
    Oracle(Oracle),
    UnexpectedPCMarker(initializeUnexpectedPCMarker(M)),
    OpaqueReturnAddress(&M, false),
    OwnCEAC(std::in_place, *M.getFunction("root")),
    CEAC(*OwnCEAC) {
    initializeOpaqueReturnAddress();
  }

  /// Outline using \p RootCEAC, an analysis cache of `root` that the caller
  /// shares among outliners: building one visits every block of `root`, and
  /// outlining leaves the blocks of `root` as they were
  Outliner(llvm::Module &M,
           RootFunction &RootF,
           const CSVGlobals &Globals,
           FunctionSummaryOracle &Oracle,
           const llvm::CodeExtractorAnalysisCache &RootCEAC) :
    M(M),
    RootF(RootF),
    Globals(Globals),
    Oracle(Oracle),
    UnexpectedPCMarker(initializeUnexpectedPCMarker(M)),
    OpaqueReturnAddress(&M, false),
    CEAC(RootCEAC) {
    initializeOpaqueReturnAddress();
  }

  Outliner(const Outliner &) = delete;
  Outliner &operator=(const Outliner &) = delete;

private:
  void initializeOpaqueReturnAddress() {
    using namespace llvm;
    auto Effects = MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref);
    OpaqueReturnAddress.setMemoryEffects(Effects);
    OpaqueReturnAddress.addFnAttribute(Attribute::NoUnwind);
    OpaqueReturnAddress.addFnAttribute(Attribute::WillReturn);
    OpaqueReturnAddress.setTags({ &FunctionTags::OpaqueReturnAddressFunction,
                                  &FunctionTags::UniquedByPrototype });
    OpaqueReturnAddress
      .initializeFromReturnType(FunctionTags::OpaqueReturnAddressFunction);
  }

public:
  OutlinedFunction outline(const MetaAddress &EntryAddress,
                           CallHandler *TheCallHandler);

private:
  static TemporaryOpaqueFunction initializeUnexpectedPCMarker(llvm::Module &M) {
    return { llvm::FunctionType::get(llvm::Type::getVoidTy(M.getContext()),
                                     false),
             "unexpectedpc_hook",
             &M };
  }

private:
  OutlinedFunction
  outlineFunctionInternal(CallHandler *TheCallHandler,
                          const MetaAddress &EntryAddress,
                          OutlinedFunctionsMap &FunctionsToInline);

  /// \return a description of the call and boolean indicating whether the call
  ///         site is a tail call or not.
  std::pair<const FunctionSummary *, bool>
  getCallSiteInfo(MetaAddress CallerFunction,
                  llvm::BasicBlock *CallerBlock,
                  llvm::CallInst *FunctionCall,
                  llvm::CallInst *JumpToSymbol,
                  MetaAddress Callee);

  void integrateFunctionCallee(CallHandler *TheCallHandler,
                               MetaAddress Caller,
                               llvm::BasicBlock *CallerBlock,
                               llvm::CallInst *FunctionCall,
                               llvm::CallInst *JumpToSymbol,
                               MetaAddress Callee,
                               OutlinedFunctionsMap &FunctionsToInline);

  llvm::Function *
  createFunctionToInline(CallHandler *TheCallHandler,
                         const MetaAddress &Entry,
                         OutlinedFunctionsMap &FunctionsToInline);

  void createAnyPCHooks(CallHandler *TheCallHandler, OutlinedFunction *F);
};

} // namespace efa
