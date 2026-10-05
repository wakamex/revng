#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <ranges>
#include <string>

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Progress.h"

#include "revng/DataLayoutAnalysis/DLALayouts.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Model/Binary.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/IRHelpers.h"

#include "../FuncOrCallInst.h"
#include "StaticFunctionPointers.h"

namespace dla {

using LayoutTypePtrVec = std::vector<LayoutTypePtr>;

inline bool isPointerToFunctionExpression(const llvm::Value *V) {
  // TODO: the current implementation DLA cannot reason about function
  // pointers, because function types have zero size.
  // Given this shortcoming, llvm::Functions are used (as a hack) to represent
  // their Functions' return types.
  // This representation is a key factor of how DLA is able to piece information
  // together interprocedurally.
  //
  // This hack has consequences.
  // Assume we're setting up the DLA graph, and we're looking at a use of an
  // llvm::Function that is inside the body of an llvm::Function and is not the
  // callee-operand of a Call.
  // That use is obviously function-pointer typed.
  // But if we try to create a LayoutTypeSystemNode for the llvm::Value of the
  // used llvm::Function, the inner machinery of how the DLA graph is set up
  // will treat is as the return type of the function, which is obviously wrong.
  // So we have to prevent that creation at all costs.
  // Basically creation of LayoutTypeSystemNodes in the DLA graph associated
  // with llvm::Functions are valid only for representing return types.
  //
  // This shortcoming does not affect the power of DLA. In its current form, it
  // doesn't represent function types at all.
  // The problem is that until we undo this hack we will not be able to properly
  // support function types in DLA. This will have to be fixed in the future.
  //
  // In the meantime, we provide this helper function to centralize how we
  // detect the offending uses of llvm::Functions. In this way, all the points
  // of the codebase that are affected by this hack are clearly marked.
  return isa<llvm::Function>(V);
}

inline bool isArrayOfBytes(const llvm::Type *T) {
  auto *Array = llvm::dyn_cast<llvm::ArrayType>(T);
  return Array and Array->getElementType()->isIntegerTy(8);
}

/// This class is used to print LLVM information when debugging the TS
///
/// Since nodes in the TypeSystem graph only have IDs, which are grouped into
/// equivalence classes, if we want to track each ID back to the original LLVM
/// Value when printing we need to define a special DebugPrinter, that knows
/// which Value is mapped to each ID.
class LLVMTSDebugPrinter : public TSDebugPrinter {
protected:
  const llvm::Module &M;
  const LayoutTypePtrVect &Values;

public:
  /// Build the Debug printer
  ///
  ///\param M The LLVM module from which the TS graph was built
  ///\param Values Ordered vector, Values are indexed with the ID of the
  ///              corresponding TypeSystemNode
  LLVMTSDebugPrinter(const llvm::Module &M, const LayoutTypePtrVect &Values) :
    M(M), Values(Values) {}

  LLVMTSDebugPrinter() = delete;

public:
  /// Print the `llvm::Value`s collapsed inside \a N
  void printNodeContent(const LayoutTypeSystem &TS,
                        const LayoutTypeSystemNode *N,
                        llvm::raw_fd_ostream &DotFile) const override;
};

/// This class builds a DLA type system from an LLVM module
class DLATypeSystemLLVMBuilder {
public:
  using VisitedMapT = std::map<LayoutTypePtr, LayoutTypeSystemNode *>;
  using FunctionsMapT = std::
    map<std::pair<const model::TypeDefinition *, std::string>, FuncOrCallInst>;
  using SegmentNodeMapT = std::map<const model::Segment *,
                                   LayoutTypeSystemNode *>;

private:
  /// Separate class that add `Instance` edges
  class InstanceLinkAdder;

  /// The TypeSystem to build
  LayoutTypeSystem &TS;

  /// The Model used by this builder to get type information.
  const model::Binary &Model;
  StaticFunctionTargets StaticTargets;
  std::map<MetaAddress, const llvm::Function *> FunctionsByEntry;
  std::map<const llvm::LoadInst *, const llvm::Function *> LoadedFunctions;

  void collectLoadedFunctions(llvm::Module &M);

  /// Ordered vector, each element is indexed with the ID of the
  /// corresponding Node
  LayoutTypePtrVect Values;

  /// Reverse map between `llvm::Value`s and Nodes
  VisitedMapT VisitedValues;

  /// Canonical declarations of the same function across modules. A shared
  /// signature alone does not identify a shared object layout.
  FunctionsMapT VisitedFunctions;

private:
  LayoutTypeSystemNode *getLayoutType(const llvm::Value *V, unsigned Id);

  LayoutTypeSystemNode *getLayoutType(const llvm::Value *V) {
    return getLayoutType(V, std::numeric_limits<unsigned>::max());
  };

  std::pair<LayoutTypeSystemNode *, bool>
  getOrCreateLayoutType(const llvm::Value *V, unsigned Id);

  std::pair<LayoutTypeSystemNode *, bool>
  getOrCreateLayoutType(const llvm::Value *V) {
    return getOrCreateLayoutType(V, dla::LayoutTypePtr::FieldNumNone);
  }

  llvm::SmallVector<LayoutTypeSystemNode *, 2>
  getLayoutTypes(const llvm::Value &V);

  llvm::SmallVector<std::pair<LayoutTypeSystemNode *, bool>, 2>
  getOrCreateLayoutTypes(const llvm::Value &V);

  /// Return the canonical direct callee when its model prototype matches.
  const llvm::Function *
  getCanonicalCallee(const model::TypeDefinition *Prototype,
                     const llvm::Function *Callee) const;

private:
  /// Create one shared artificial node per model segment.
  void initializeSegments(SegmentNodeMapT &Segments);
  bool createInterproceduralTypes(llvm::Module &M,
                                  const SegmentNodeMapT &Segments);
  bool createIntraproceduralTypes(llvm::Module &M);

  /// Collect LayoutTypePtrs and place them in the right position
  void createValuesList();

public:
  LayoutTypePtrVect &getValues() { return Values; }

  /// Print a `.csv` with the mapping between nodes and `llvm::Value`s
  ///
  /// The mapping is reconstructed on-the-fly, therefore is expensive. The
  /// generated .csv uses *semicolons* as separators.
  void debug_function dumpValuesMapping(const llvm::StringRef Name) const;

public:
  DLATypeSystemLLVMBuilder(LayoutTypeSystem &TS,
                           const model::Binary &M,
                           StaticFunctionTargets Targets = {}) :
    TS(TS), Model(M), StaticTargets(std::move(Targets)) {}

  /// Create a DLATypeSystem graph from a range of LLVM modules
  ///
  /// LayoutTypePtrs represent elements of the LLVM IR that are thought to be
  /// possible pointers. The builder's job is to:
  /// 1. Identify such LayoutTypePtrs
  /// 2. Create a Node for each of them in the DLATypeSystem graph (TS)
  /// 3. Keep an ordered vector of LayoutTypePtrs, where each element's index
  /// corresponds to the ID of the corresponding LayoutTypeSystemNode generated
  ///
  /// \p Modules is a multipass range of `llvm::Module *`. VisitedValues and
  /// VisitedFunctions are kept alive across all the modules, so that values
  /// and prototypes shared between modules converge on the same nodes.
  /// \p ModuleNames names each module in the progress steps.
  template<typename ModuleRange>
  void buildFromLLVMModules(ModuleRange &&Modules,
                            llvm::ArrayRef<std::string> ModuleNames) {
    revng_assert(static_cast<size_t>(std::ranges::distance(Modules))
                 == ModuleNames.size());

    // The debug printer only needs a module handle for debugging convenience,
    // so install it with the first module of the range. For an empty request
    // the default base printer installed by LayoutTypeSystem stands.
    auto It = std::ranges::begin(Modules);
    if (It != std::ranges::end(Modules))
      TS.setDebugPrinter(std::make_unique<LLVMTSDebugPrinter>(**It,
                                                              this->Values));

    // Create the shared per-segment nodes once, before any module is visited,
    // so getters from different modules connect to the same node.
    SegmentNodeMapT SegmentNodes;
    initializeSegments(SegmentNodes);

    if (not StaticTargets.empty()) {
      for (llvm::Module *M : Modules)
        for (const llvm::Function &F : *M)
          if (FunctionTags::Isolated.isTagOf(&F)) {
            auto Entry = getMetaAddressMetadata(&F, FunctionEntryMDName);
            if (Entry.isValid())
              FunctionsByEntry.try_emplace(Entry, &F);
          }
      for (llvm::Module *M : Modules)
        collectLoadedFunctions(*M);
    }

    llvm::Task T(3, "DLA type system");
    T.advance("Create interprocedural types");
    {
      llvm::Task PerModule(ModuleNames.size(), "Create interprocedural types");
      for (auto &&[M, Name] : llvm::zip_equal(Modules, ModuleNames)) {
        PerModule.advance(Name);
        createInterproceduralTypes(*M, SegmentNodes);
      }
    }

    T.advance("Create intraprocedural types");
    {
      llvm::Task PerModule(ModuleNames.size(), "Create intraprocedural types");
      for (auto &&[M, Name] : llvm::zip_equal(Modules, ModuleNames)) {
        PerModule.advance(Name);
        createIntraproceduralTypes(*M);
      }
    }

    T.advance("Create values list");
    createValuesList();
    VisitedValues.clear();
    VisitedFunctions.clear();
    FunctionsByEntry.clear();
    LoadedFunctions.clear();
  }
};

} // namespace dla
