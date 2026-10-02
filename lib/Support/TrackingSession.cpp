//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <memory>

#include "llvm/ADT/STLExtras.h"

#include "revng/Support/TrackingSession.h"
#include "revng/TupleTree/TupleTreePath.h"

namespace revng {

std::atomic<unsigned> detail::OpenTrackingSessions = 0;

static thread_local std::unique_ptr<TrackingSession> ThreadSession;

static TrackingSessionMembership &
membershipOf(const TrackingSession::Object &Container) {
  return Container.Kind->membership(Container);
}

TrackingSession *TrackingSession::find(const void *Root) {
  TrackingSession *Session = ThreadSession.get();
  if (Session != nullptr and Session->Root == Root)
    return Session;
  return nullptr;
}

TrackingSession *TrackingSession::open(const void *Root) {
  if (ThreadSession)
    return nullptr;

  ThreadSession.reset(new TrackingSession(Root));
  ++detail::OpenTrackingSessions;
  return ThreadSession.get();
}

void TrackingSession::close(const void *Root) {
  TrackingSession *Session = find(Root);
  if (Session == nullptr)
    return;

  for (ListedTracker &Listed : Session->Trackers)
    Listed.Tracker->membership().set(TrackingSessionMembership::Unknown);

  for (uint32_t Index : Session->Containers) {
    const Object &Container = Session->Objects[Index];
    membershipOf(Container).set(TrackingSessionMembership::Unknown);
  }

  ThreadSession.reset();
  --detail::OpenTrackingSessions;
}

void TrackingSession::listTracker(AccessTracker &Tracker,
                                  uint32_t Object,
                                  int32_t Field) {
  revng_assert(Tracker.membership().get() != TrackingSessionMembership::Listed);
  Tracker.membership().set(TrackingSessionMembership::Listed);
  Trackers.push_back({ &Tracker, Object, Field });
}

void TrackingSession::finishIndex() {
  ByAddress.resize(Objects.size());
  for (uint32_t I = 0; I < Objects.size(); ++I)
    ByAddress[I] = I;

  // An object enclosing another one may start at the same address: sort the
  // larger first, so that the last object starting at or before an address is
  // the innermost one starting there
  llvm::sort(ByAddress, [this](uint32_t LHS, uint32_t RHS) {
    const Object &L = Objects[LHS];
    const Object &R = Objects[RHS];
    if (L.Begin != R.Begin)
      return L.Begin < R.Begin;
    return L.Size > R.Size;
  });
}

uint32_t TrackingSession::objectAt(const void *Address) const {
  const char *Target = static_cast<const char *>(Address);
  auto It = std::upper_bound(ByAddress.begin(),
                             ByAddress.end(),
                             Target,
                             [this](const char *Target, uint32_t Index) {
                               return Target < Objects[Index].Begin;
                             });
  if (It == ByAddress.begin())
    return None;

  // The last object starting at or before Target either contains it, or is
  // nested in the object that does
  uint32_t Index = *std::prev(It);
  while (Index != None) {
    const Object &Candidate = Objects[Index];
    if (Candidate.Begin <= Target and Target < Candidate.Begin + Candidate.Size)
      return Index;
    Index = Candidate.Enclosing;
  }

  return None;
}

void TrackingSession::onFirstAccess(AccessTracker &Tracker) {
  uint32_t Index = objectAt(&Tracker);
  int32_t Field = NoField;
  if (Index != None)
    Field = Objects[Index].Kind->fieldOf(Objects[Index], &Tracker);

  if (Field == NoField)
    Tracker.membership().set(TrackingSessionMembership::Foreign);
  else
    listTracker(Tracker, Index, Field);
}

size_t
TrackingSession::implicitLevels(const void *Container,
                                TrackingSessionMembership &Membership) {
  using Membership_t = TrackingSessionMembership;
  revng_assert(Membership.get() != Membership_t::Listed);
  if (Membership.get() == Membership_t::Foreign)
    return 1;

  uint32_t Index = objectAt(Container);
  if (Index == None or Objects[Index].Begin != Container
      or not Objects[Index].isContainer()) {
    Membership.set(Membership_t::Foreign);
    return 1;
  }

  Membership.set(Membership_t::Listed);
  Containers.push_back(Index);
  return ContainerLevels;
}

void TrackingSession::push() {
  for (ListedTracker &Listed : Trackers)
    Listed.Tracker->push();

  for (uint32_t Index : Containers)
    Objects[Index].Kind->pushLevel(Objects[Index]);

  ++ContainerLevels;
}

void TrackingSession::pop() {
  // A tracker whose state becomes empty, or a container left with the
  // implicit empty levels, no longer needs to be listed
  llvm::erase_if(Trackers, [](ListedTracker &Listed) {
    Listed.Tracker->pop();
    if (Listed.Tracker->isSet())
      return false;
    Listed.Tracker->membership().set(TrackingSessionMembership::Unknown);
    return true;
  });

  ContainerLevels = std::max<size_t>(ContainerLevels - 1, 1);

  llvm::erase_if(Containers, [this](uint32_t Index) {
    const Object &Container = Objects[Index];
    Container.Kind->popLevel(Container);
    if (not Container.Kind->hasEmptyLevels(Container, ContainerLevels))
      return false;
    Container.Kind->forgetLevels(Container);
    membershipOf(Container).set(TrackingSessionMembership::Unknown);
    return true;
  });
}

void TrackingSession::appendPath(uint32_t Index, TupleTreePath &Path) const {
  const Object &Target = Objects[Index];
  if (Target.Parent != None)
    appendPath(Target.Parent, Path);
  Target.Kind->appendElements(Target, Path);
}

void TrackingSession::collect(llvm::SmallVectorImpl<TupleTreePath> &Read,
                              llvm::SmallVectorImpl<TupleTreePath> &Exact) {
  for (const ListedTracker &Listed : Trackers) {
    if (not Listed.Tracker->isSet())
      continue;

    TupleTreePath Path;
    appendPath(Listed.Object, Path);
    if (Listed.Field == ExactField) {
      Exact.push_back(std::move(Path));
    } else {
      Path.push_back(static_cast<size_t>(Listed.Field));
      Read.push_back(std::move(Path));
    }
  }

  for (uint32_t Index : Containers) {
    TupleTreePath Path;
    appendPath(Index, Path);
    Objects[Index].Kind->collectKeys(Objects[Index], Path, Read);
  }

  clear();
}

void TrackingSession::clear() {
  // Clear the innermost level of the trackers, and every level of the
  // containers, as clearing tracking on the whole tree does
  llvm::erase_if(Trackers, [](ListedTracker &Listed) {
    Listed.Tracker->clear();
    if (Listed.Tracker->isSet())
      return false;
    Listed.Tracker->membership().set(TrackingSessionMembership::Unknown);
    return true;
  });

  for (uint32_t Index : Containers) {
    const Object &Container = Objects[Index];
    Container.Kind->forgetLevels(Container);
    membershipOf(Container).set(TrackingSessionMembership::Unknown);
  }
  Containers.clear();
  ContainerLevels = 1;
}

void detail::onFirstAccess(AccessTracker &Tracker) {
  if (ThreadSession)
    ThreadSession->onFirstAccess(Tracker);
}

size_t detail::implicitLevels(const void *Container,
                              TrackingSessionMembership &Membership) {
  if (ThreadSession)
    return ThreadSession->implicitLevels(Container, Membership);
  return 1;
}

} // namespace revng
