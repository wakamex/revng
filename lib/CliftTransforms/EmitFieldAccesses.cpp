//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "revng/Clift/Clift.h"
#include "revng/CliftTransforms/EmitFieldAccesses.h"
#include "revng/CliftTransforms/Passes.h"

#include "EmitFieldAccesses/BestTraversal.h"
#include "EmitFieldAccesses/FieldAccessReplacement.h"
#include "EmitFieldAccesses/PointerArithmetic.h"

namespace clift {
#define GEN_PASS_DEF_CLIFTEMITFIELDACCESSES
#include "revng/CliftTransforms/Passes.h.inc"
} // namespace clift

using namespace clift;

namespace {

static thread_local TraversalCache *ThreadTraversalCache;

struct EmitFieldAccessesPattern
  : mlir::OpInterfaceRewritePattern<ExpressionOpInterface> {

  EmitFieldAccessesPattern(mlir::MLIRContext *Context,
                           mlir::PatternBenefit Benefit = 1) :
    OpInterfaceRewritePattern(Context, Benefit) {}

  void initialize() { setDebugName("emit-field-accesses"); }

  mlir::LogicalResult
  matchAndRewrite(ExpressionOpInterface Op,
                  mlir::PatternRewriter &Rewriter) const override {
    std::optional<TraversalCache> LocalCache;
    TraversalCache *Cache = ThreadTraversalCache;

    if (Cache == nullptr)
      Cache = &LocalCache.emplace();

    if (std::optional<PointerArithmetic> PA = computePointerArithmetic(Op)) {
      if (std::optional<Traversal> BT = computeBestTraversal(Op, *PA, *Cache)) {

        if (replaceFieldAccess(Rewriter, Op, *PA, *BT))
          return mlir::success();
      }
    }

    return mlir::failure();
  }
};

struct EmitFieldAccessesPass
  : clift::impl::CliftEmitFieldAccessesBase<EmitFieldAccessesPass> {

  mlir::FrozenRewritePatternSet Patterns;

  /// The traversals of each base type and offset, kept across this pass's
  /// runs: a pass manager runs one pass object on each function's module, and
  /// the traversals depend only on the type, uniqued in its context. Rebuilt
  /// for each function, the walks of the same large types took most of a
  /// whole program's emission. Cleared once it holds `MaxCachedEntries`.
  TraversalCache PassCache;
  static constexpr size_t MaxCachedEntries = size_t(1) << 20;

  mlir::LogicalResult initialize(mlir::MLIRContext *Context) override {
    mlir::RewritePatternSet Set(Context);
    populateWithEmitFieldAccessesPatterns(Set);
    Patterns = mlir::FrozenRewritePatternSet(std::move(Set));

    return mlir::success();
  }

  void runOnOperation() override {
    // A caller's cache, if it set one, is used instead
    bool OwnCache = ThreadTraversalCache == nullptr;
    if (OwnCache) {
      if (PassCache.Traversals.size() + PassCache.ArrayPaths.size()
          > MaxCachedEntries)
        PassCache = TraversalCache();
      ThreadTraversalCache = &PassCache;
    }

    mlir::GreedyRewriteConfig Config;
    Config.maxIterations = mlir::GreedyRewriteConfig::kNoLimit;
    if (mlir::applyPatternsAndFoldGreedily(getOperation(), Patterns, Config)
          .failed())
      signalPassFailure();

    if (OwnCache)
      ThreadTraversalCache = nullptr;
  }
};

} // namespace

struct EFAThreadCache::ImplType {
  TraversalCache Cache;

  ImplType() {
    revng_assert(ThreadTraversalCache == nullptr);
    ThreadTraversalCache = &Cache;
  }

  ~ImplType() {
    revng_assert(ThreadTraversalCache == &Cache);
    ThreadTraversalCache = nullptr;
  }
};

EFAThreadCache::EFAThreadCache() : Impl(std::make_unique<ImplType>()) {
}

EFAThreadCache::~EFAThreadCache() = default;

void populateWithEmitFieldAccessesPatterns(mlir::RewritePatternSet &Set) {
  Set.add<EmitFieldAccessesPattern>(Set.getContext());
}

PassPtr<FunctionOp> clift::createEmitFieldAccessesPass() {
  return std::make_unique<EmitFieldAccessesPass>();
}
