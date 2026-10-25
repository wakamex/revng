#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <set>

#include "revng/TupleTree/TupleTreePath.h"

/// Struct returned by Tracking::collect.
/// The field Read is the set of paths of fields that were accessed.
/// The exact vectors contains the paths of all vectors that were marked as
/// requiring being identical.
///
/// We divide into read and exact vectors because when we will introduce the
/// cold start invalidation based on the hash, the hash will be different
/// depending if they are just read or if they are vectors required to be exact.
struct ReadFields {
  llvm::SmallVector<TupleTreePath> Read;
  llvm::SmallVector<TupleTreePath> ExactVectors;
  /// Top-level fields of the root, each a path of one element, whose reads
  /// were too many to record one by one. Reading under such a field records
  /// the field itself, and any change under it invalidates the reader.
  llvm::SmallVector<TupleTreePath> Coarse;

  /// A top-level field with more reads than this is recorded as a whole. A
  /// function whose types reach most of a large model reads millions of
  /// fields, and recording each costs far more than the work itself.
  static constexpr size_t CoarseThreshold = 10000;

  /// Replace the reads under each top-level field read more than
  /// CoarseThreshold times by that field
  void coarsen() {
    auto Prefix = [](const TupleTreePath &Path) {
      TupleTreePath Result = Path;
      Result.resize(1);
      return Result;
    };
    std::map<TupleTreePath, size_t> Counts;
    for (const TupleTreePath &Path : Read)
      if (Path.size() > 1)
        ++Counts[Prefix(Path)];
    for (const TupleTreePath &Path : ExactVectors)
      if (Path.size() > 1)
        ++Counts[Prefix(Path)];

    for (const auto &[Top, Count] : Counts)
      if (Count > CoarseThreshold)
        Coarse.push_back(Top);
    if (Coarse.empty())
      return;

    auto IsCoarse = [this](const TupleTreePath &Path) {
      return llvm::any_of(Coarse, [&Path](const TupleTreePath &Top) {
        return Top.isPrefixOf(Path);
      });
    };
    llvm::erase_if(Read, IsCoarse);
    llvm::erase_if(ExactVectors, IsCoarse);
  }

  void deduplicate() {
    llvm::sort(Read);
    Read.erase(std::unique(Read.begin(), Read.end()), Read.end());
    llvm::sort(ExactVectors);
    ExactVectors.erase(std::unique(ExactVectors.begin(), ExactVectors.end()),
                       ExactVectors.end());
    llvm::sort(Coarse);
    Coarse.erase(std::unique(Coarse.begin(), Coarse.end()), Coarse.end());
  }
};

namespace revng {

struct Tracking {
  template<typename M>
  static ReadFields collect(const M &LHS);

  template<typename M>
  static void clearAndResume(const M &LHS);

  template<typename M>
  static void push(const M &LHS);

  template<typename M>
  static void pop(const M &LHS);

  /// Stop tracking, and close the TrackingSession open on \p LHS, if any
  template<typename M>
  static void stop(const M &LHS);

  /// Open a TrackingSession on \p LHS, unless this thread has one already:
  /// until stop(LHS), push, pop, clearAndResume and collect on LHS visit only
  /// the trackers and containers that hold state, with the same results.
  /// \p LHS must not change shape, nor be destroyed, until then.
  template<typename M>
  static void openSession(const M &LHS);
};

} // namespace revng
