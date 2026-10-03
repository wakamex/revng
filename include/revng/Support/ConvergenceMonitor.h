#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace revng {

/// A worklist loop that is not converging: the items involved and why.
struct NonConvergence {
  struct Item {
    std::string Name;
    uint64_t Visits = 0;
    /// Visits that reported a change but left the item in a state it already
    /// had
    uint64_t Repeats = 0;
    /// The repeated state, the visit that first produced it and the latest
    /// visit that returned to it
    uint64_t State = 0;
    uint64_t FirstVisit = 0;
    uint64_t RepeatVisit = 0;
  };

  std::string Loop;
  std::string Reason;
  uint64_t Visits = 0;
  std::vector<Item> Items;

  /// A one-line description, safe to embed in a trace event name
  std::string describe() const;
};

using NonConvergenceHandler = std::function<void(const NonConvergence &)>;

/// Replace what happens when a loop is found not to converge, returning the
/// previous handler. The default handler emits a trace event and then fails
/// or warns according to -convergence-check. Tests install a handler that
/// throws.
NonConvergenceHandler setNonConvergenceHandler(NonConvergenceHandler Handler);

/// Hand \p Failure to the current handler.
void reportNonConvergence(const NonConvergence &Failure);

/// Whether -convergence-check enables detection at all.
bool convergenceChecksEnabled();

/// Emit a zero-length task named \p Name in the trace, so that readers
/// following the trace see it at the position it happened.
void emitTraceEvent(llvm::StringRef Name);

/// Watches a worklist loop whose items are re-enqueued when they change, and
/// detects when it cannot converge.
///
/// The loop reports each visit with a hash of the state that decides whether
/// the item changed. In a loop that converges, a visit that reports a change
/// leaves the item in a state it never had before: summaries only grow, so
/// returning to an earlier state means the change test disagrees with the
/// state, as a comparison inconsistent with the set's order does. Such an
/// item is re-enqueued, and re-enqueues its dependents, forever. The monitor
/// counts these repeats and reports the loop once an item reaches
/// -convergence-repeats of them, or once an item exceeds
/// -convergence-visit-bound visits.
///
/// The loop's visit, item and change counts go to the trace as "Convergence
/// progress" events: at most one a second, one at each repeat, and a final one
/// from the destructor.
class ConvergenceMonitor {
private:
  struct ItemState {
    uint64_t Visits = 0;
    uint64_t Repeats = 0;
    bool EverChanged = false;
    /// Each state the item has had, with the first visit that produced it
    llvm::SmallDenseMap<uint64_t, uint64_t, 4> States;
    uint64_t RepeatState = 0;
    uint64_t RepeatFirstVisit = 0;
    uint64_t RepeatVisit = 0;
  };

private:
  std::string Loop;
  bool Enabled;
  llvm::StringMap<ItemState> Items;
  uint64_t Visits = 0;
  uint64_t ChangedVisits = 0;
  uint64_t ChangedItems = 0;
  uint64_t Repeats = 0;
  uint64_t MaxVisits = 0;
  std::string MaxItem;
  bool Reported = false;
  std::chrono::steady_clock::time_point LastProgress;

public:
  explicit ConvergenceMonitor(llvm::StringRef Loop);
  ~ConvergenceMonitor();

  ConvergenceMonitor(const ConvergenceMonitor &) = delete;
  ConvergenceMonitor &operator=(const ConvergenceMonitor &) = delete;

public:
  /// Record that the loop visited \p Item, which is now in state \p State,
  /// and whether the loop considered it changed.
  void visit(llvm::StringRef Item, uint64_t State, bool Changed);

  uint64_t visits() const { return Visits; }
  uint64_t repeats() const { return Repeats; }

private:
  std::string progress() const;
  NonConvergence failure(llvm::StringRef Reason) const;
};

} // namespace revng
