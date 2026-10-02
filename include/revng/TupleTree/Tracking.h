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

  void deduplicate() {
    llvm::sort(Read);
    Read.erase(std::unique(Read.begin(), Read.end()), Read.end());
    llvm::sort(ExactVectors);
    ExactVectors.erase(std::unique(ExactVectors.begin(), ExactVectors.end()),
                       ExactVectors.end());
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
