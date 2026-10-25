#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <limits>
#include <vector>

#include "llvm/ADT/SmallVector.h"

#include "revng/Support/AccessTracker.h"

class TupleTreePath;

namespace revng {

/// Tracking of the reads of a tuple tree (see revng/TupleTree/Tracking.h)
/// pushes, pops, clears and collects the state of every tracker and keyed
/// container of the tree. A pipe recording the reads of each function does so
/// several times per function, so the whole tree was visited several times per
/// function.
///
/// A TrackingSession is open on the root of a tree from the moment tracking
/// resumes on it until it stops. It indexes the objects of the tree by address
/// and lists the trackers and containers that hold any state, so that those
/// operations on the root visit only the listed ones and build the paths of
/// the read fields from the index, with the same result as visiting the tree.
///
/// The tree must not change shape while the session is open. At most one
/// session is open per thread, and trackers of other trees, or of other
/// threads, behave as without a session.
class TrackingSession {
public:
  static constexpr uint32_t None = std::numeric_limits<uint32_t>::max();
  /// Field of the Exact tracker of a container
  static constexpr int32_t ExactField = -1;
  /// Not a tracker of the object
  static constexpr int32_t NoField = -2;

  struct Object;

  /// Operations on an object of a given type, provided by TrackingImpl
  struct ObjectKind {
    /// Append the path elements leading from the parent to the object
    void (*appendElements)(const Object &, TupleTreePath &);
    /// The field that \p Tracker tracks in the object, ExactField if it is the
    /// Exact tracker of a container, NoField if neither
    int32_t (*fieldOf)(const Object &, const AccessTracker *Tracker);

    /// \defgroup Container operations, null for structs
    /// @{
    void (*pushLevel)(const Object &);
    void (*popLevel)(const Object &);
    void (*forgetLevels)(const Object &);
    bool (*hasEmptyLevels)(const Object &, size_t Levels);
    TrackingSessionMembership &(*membership)(const Object &);
    /// Append to \p Read the path of each key inspected in the container
    void (*collectKeys)(const Object &,
                        const TupleTreePath &Path,
                        llvm::SmallVectorImpl<TupleTreePath> &Read);
    /// @}
  };

  struct Object {
    /// The storage of the object
    const char *Begin = nullptr;
    size_t Size = 0;
    /// The value the object was reached through: the object itself, or the
    /// UpcastablePointer that points to it
    const void *Source = nullptr;
    /// The object whose path the object's path extends
    uint32_t Parent = None;
    /// The object whose storage contains this object's storage
    uint32_t Enclosing = None;
    /// The field of the parent that holds the object, if the parent is a
    /// struct
    uint32_t Field = None;
    const ObjectKind *Kind = nullptr;

    bool isContainer() const { return Kind->pushLevel != nullptr; }
  };

  struct ListedTracker {
    AccessTracker *Tracker;
    uint32_t Object;
    int32_t Field;
  };

private:
  const void *Root;
  std::vector<Object> Objects;
  /// Object indices sorted by address, enclosing objects first
  std::vector<uint32_t> ByAddress;
  /// For each object, the top-level field of the root it lies in (the object
  /// whose parent is the root), None for the root
  std::vector<uint32_t> TopLevel;
  std::vector<ListedTracker> Trackers;
  /// Containers whose levels are materialized
  std::vector<uint32_t> Containers;
  /// The number of levels of a container whose levels are implicit
  size_t ContainerLevels = 1;

private:
  TrackingSession(const void *Root) : Root(Root) {}

public:
  /// The session open on \p Root in this thread, if any
  static TrackingSession *find(const void *Root);
  /// Open a session on \p Root, unless this thread has one open already.
  /// Once its objects are indexed, the caller must call finishIndex.
  static TrackingSession *open(const void *Root);
  /// Close the session open on \p Root in this thread, if any
  static void close(const void *Root);

public:
  /// \defgroup Building the index
  /// @{
  uint32_t addObject(const Object &NewObject) {
    Objects.push_back(NewObject);
    return Objects.size() - 1;
  }

  /// List a tracker that holds state when the session opens
  void listTracker(AccessTracker &Tracker, uint32_t Object, int32_t Field);

  void finishIndex();
  /// @}

public:
  /// List \p Tracker if it belongs to the tree
  void onFirstAccess(AccessTracker &Tracker);

  /// The number of levels a container of the tree has while its levels are
  /// implicit, listing it, since it is about to materialize them; 1 for a
  /// container outside of the tree
  size_t implicitLevels(const void *Container,
                        TrackingSessionMembership &Membership);

  /// \defgroup Operations on the whole tree
  /// @{
  void push();
  void pop();
  /// Collect the paths of the read fields and of the inspected keys, the
  /// paths of the containers read as a whole, then clear the innermost level
  /// of the trackers and every level of the containers. The reads under a
  /// top-level field of the root with more than \p CoarseThreshold read
  /// trackers and inspected containers are not built: the field's path goes
  /// to \p Coarse instead.
  void collect(llvm::SmallVectorImpl<TupleTreePath> &Read,
               llvm::SmallVectorImpl<TupleTreePath> &ExactVectors,
               llvm::SmallVectorImpl<TupleTreePath> &Coarse,
               size_t CoarseThreshold);
  /// Clear the innermost level of the trackers and every level of the
  /// containers
  void clear();
  /// @}

  size_t objectCount() const { return Objects.size(); }
  size_t listedTrackerCount() const { return Trackers.size(); }

private:
  uint32_t objectAt(const void *Address) const;
  void appendPath(uint32_t Index, TupleTreePath &Path) const;
};

namespace detail {

/// See TrackingSession::implicitLevels
size_t implicitLevels(const void *Container,
                      TrackingSessionMembership &Membership);

} // namespace detail

} // namespace revng
