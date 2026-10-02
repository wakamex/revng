#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <atomic>
#include <cstdint>

#include "llvm/ADT/StringRef.h"

#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

void onFieldAccess(llvm::StringRef FieldName,
                   llvm::StringRef StructName) debug_function;

namespace revng {

class AccessTracker;

/// Membership of a tracked object (an AccessTracker or a TrackingContainer) in
/// the open TrackingSession of this thread, if any.
///
/// A copy is a different object and starts as Unknown, while an assignment
/// changes the content of an object but not its identity, so it keeps the
/// membership of the target.
class TrackingSessionMembership {
public:
  enum Values : std::uint8_t {
    /// Not looked up in the open session yet
    Unknown,
    /// Part of the tree of the open session, which lists it
    Listed,
    /// Not part of the tree of the open session
    Foreign
  };

private:
  Values Value = Unknown;

public:
  TrackingSessionMembership() = default;
  TrackingSessionMembership(const TrackingSessionMembership &) {}
  TrackingSessionMembership &operator=(const TrackingSessionMembership &) {
    return *this;
  }

public:
  Values get() const { return Value; }
  void set(Values NewValue) { Value = NewValue; }
};

namespace detail {

/// Number of TrackingSession open in any thread: while it is zero, accesses
/// never look for a session
extern std::atomic<unsigned> OpenTrackingSessions;

/// Called on the first access to \p Tracker while a session is open: lists it
/// in this thread's session if it belongs to its tree
void onFirstAccess(AccessTracker &Tracker);

} // namespace detail

///
/// A AccessCounter is optimized std::stack<bool> that can contain up to 8
/// elements.
///
class AccessTracker {
private:
  std::uint8_t Counter = 0;
  bool IsTracking = false;
  mutable TrackingSessionMembership Membership;

public:
  class TrackingSuspender {
  private:
    AccessTracker &Tracker;
    bool WasTracking = false;

  public:
    TrackingSuspender(AccessTracker &Tracker) : Tracker(Tracker) {
      WasTracking = Tracker.IsTracking;
      Tracker.IsTracking = false;
    }

    ~TrackingSuspender() { Tracker.IsTracking = WasTracking; }

    TrackingSuspender(const TrackingSuspender &) = delete;
    TrackingSuspender(TrackingSuspender &&) = delete;
    TrackingSuspender &operator=(const TrackingSuspender &) = delete;
    TrackingSuspender &operator=(TrackingSuspender &&) = delete;
  };

public:
  AccessTracker(bool StartsActive) { IsTracking = StartsActive; }

public:
  bool operator==(const AccessTracker &Other) const {
    return Counter == Other.Counter and IsTracking == Other.IsTracking;
  }

public:
  void clear() {
    Counter &= ~0x1;
    IsTracking = true;
  }

  void access() {
    if (not IsTracking)
      return;

    Counter |= 0x1;
    if (Membership.get() == TrackingSessionMembership::Unknown
        and detail::OpenTrackingSessions.load(std::memory_order_relaxed) != 0)
      detail::onFirstAccess(*this);
  }

  void push() {
    bool HasLeadingZeroes = llvm::countLeadingZeros(Counter) != 0;
    revng_assert(HasLeadingZeroes, "More than 8 pushes have been performed");
    Counter = Counter << 1;
  }

  void pop() { Counter = Counter >> 1; }
  bool front() const { return (Counter & 0x1) == 0; }
  bool peak() const { return Counter & 0x1; }
  bool isSet() const { return Counter; }
  void stopTracking() { IsTracking = false; }

  TrackingSuspender suspend() { return TrackingSuspender(*this); }

  TrackingSessionMembership &membership() const { return Membership; }
};

} // namespace revng
