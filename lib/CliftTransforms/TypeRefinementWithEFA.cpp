//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "revng/CliftTransforms/EmitFieldAccesses.h"
#include "revng/CliftTransforms/Expressions.h"
#include "revng/CliftTransforms/Passes.h"
#include "revng/CliftTransforms/TypeRefinement.h"

namespace clift {
#define GEN_PASS_DEF_CLIFTTYPEREFINEMENTWITHEFA
#include "revng/CliftTransforms/Passes.h.inc"
} // namespace clift

using namespace clift;

namespace {

struct TypeRefinementWithEFAPass
  : clift::impl::CliftTypeRefinementWithEFABase<TypeRefinementWithEFAPass> {

  mlir::FrozenRewritePatternSet RefinementPatterns;
  mlir::FrozenRewritePatternSet Patterns;

  mlir::LogicalResult initialize(mlir::MLIRContext *Context) override {
    mlir::RewritePatternSet RefinementSet(Context);
    populateWithTypeRefinementPatterns(RefinementSet);
    populateWithLocalStorageAccessPatterns(RefinementSet);

    mlir::RewritePatternSet Set(Context);

    populateWithEmitFieldAccessesPatterns(Set);
    populateWithTypeRefinementPatterns(Set);
    populateWithExpressionOptimizationPatterns(Set);

    llvm::SmallVector<std::string> DisabledPatterns(disabledPatterns.begin(),
                                                    disabledPatterns.end());

    // EFA cannot handle simplification of certain subscript-related rewrites.
    // Applying such rewrite would cause the pass to never converge due to
    // non-idempotence of EFA.
    //
    // TODO: Fix EFA and re-enable the relevant rewrite patterns.
    DisabledPatterns.emplace_back("incompatible-with-efa");

    // A local takes the type of what is assigned to it before the type of a
    // cast at one of its reads, which only says how that read uses it
    llvm::SmallVector<std::string> ForwardOnly(DisabledPatterns);
    ForwardOnly.emplace_back("backward-type-refinement");
    RefinementPatterns = mlir::FrozenRewritePatternSet(std::move(RefinementSet),
                                                       ForwardOnly,
                                                       enabledPatterns);
    Patterns = mlir::FrozenRewritePatternSet(std::move(Set),
                                             DisabledPatterns,
                                             enabledPatterns);

    return mlir::success();
  }

  void runOnOperation() override {
    [[maybe_unused]] EFAThreadCache EFACache;

    mlir::GreedyRewriteConfig Config;
    Config.maxIterations = mlir::GreedyRewriteConfig::kNoLimit;

    // Refine types forward before emitting field accesses: EFA rewrites an
    // access through a byte pointer into a subscript, which it does not revisit
    // once refinement gives the pointer a struct type, as it does for a local
    // assigned a pointer to a struct. This phase runs no other rewrite, so
    // that EFA still sees every other access in the form it expects.
    if (mlir::applyPatternsAndFoldGreedily(getOperation(),
                                           RefinementPatterns,
                                           Config)
          .failed()) {
      signalPassFailure();
      return;
    }

    if (mlir::applyPatternsAndFoldGreedily(getOperation(), Patterns, Config)
          .failed())
      signalPassFailure();
  }
};

} // namespace

PassPtr<clift::FunctionOp> clift::createTypeRefinementWithEFAPass() {
  return std::make_unique<TypeRefinementWithEFAPass>();
}
