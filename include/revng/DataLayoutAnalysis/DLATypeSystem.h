#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <compare>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/IntEqClasses.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

namespace dla {

/// Class used to mark InstanceLinkTags between LayoutTypes
struct OffsetExpression {
  uint64_t Offset = 0;
  llvm::SmallVector<uint64_t, 4> Strides;
  llvm::SmallVector<std::optional<uint64_t>, 4> TripCounts;

  explicit OffsetExpression() : OffsetExpression(0ULL){};
  explicit OffsetExpression(uint64_t Off) :
    Offset(Off), Strides(), TripCounts() {}

  OffsetExpression(const OffsetExpression &) = default;
  OffsetExpression &operator=(const OffsetExpression &) = default;

  OffsetExpression(OffsetExpression &&) = default;
  OffsetExpression &operator=(OffsetExpression &&) = default;

  std::strong_ordering
  operator<=>(const OffsetExpression &Other) const = default;

  void print(llvm::raw_ostream &OS) const;

  bool verify() const debug_function {
    if (Offset < 0)
      return false;

    if (Strides.size() != TripCounts.size())
      return false;

    uint64_t PrevStride = std::numeric_limits<uint64_t>::max();
    for (const auto &[Stride, MaybeTC] : llvm::zip_first(Strides, TripCounts)) {

      // Strides should go from larger to smaller
      if (PrevStride < Stride)
        return false;

      // Arrays with unknown length are considered as if they had one element
      auto TripCount = MaybeTC.value_or(1);
      // If the current stride times the current trip count is larger than the
      // previous stride, it would trip over the element of the outer array.
      if (Stride * TripCount > PrevStride)
        return false;

      PrevStride = Stride;
    }

    return true;
  }

  static OffsetExpression append(OffsetExpression LHS,
                                 const OffsetExpression &RHS) {
    revng_assert(LHS.verify());
    revng_assert(RHS.verify());
    LHS.Offset += RHS.Offset;
    LHS.Strides.append(RHS.Strides);
    LHS.TripCounts.append(RHS.TripCounts);
    revng_assert(LHS.verify());
    return LHS;
  }

}; // end class OffsetExpression

class TypeLinkTag {
public:
  enum LinkKind {
    LK_Equality,
    LK_Instance,
    LK_Pointer,
    LK_All,
  };

  static const char *toString(enum LinkKind K) {
    switch (K) {
    case LK_Equality:
      return "Equality";
    case LK_Instance:
      return "Instance";
    case LK_Pointer:
      return "Pointer";
    case LK_All:
      return "None";
    }
    revng_unreachable();
  }

protected:
  const LinkKind Kind;
  OffsetExpression OE;

  explicit TypeLinkTag(LinkKind K, OffsetExpression &&O) :
    Kind(K), OE(std::move(O)) {}

  // TODO: potentially we are interested in marking TypeLinkTags with some info
  // that allows us to track which step on the type system has created them.
  // However, this is not necessary now, so I'll leave it for when we have
  // identified more clearly if we really need it and why.

public:
  TypeLinkTag() = delete;

  LinkKind getKind() const { return Kind; }

  const OffsetExpression &getOffsetExpr() const {
    revng_assert(getKind() == LK_Instance);
    return OE;
  }

  static TypeLinkTag equalityTag() {
    return TypeLinkTag(LK_Equality, OffsetExpression{});
  }

  // This method is templated just to enable perfect forwarding.
  template<typename OffsetExpressionT>
  static TypeLinkTag instanceTag(OffsetExpressionT &&O) {
    return TypeLinkTag(LK_Instance, std::forward<OffsetExpressionT>(O));
  }

  static TypeLinkTag pointerTag() {
    return TypeLinkTag(LK_Pointer, OffsetExpression{});
  }

  std::strong_ordering operator<=>(const TypeLinkTag &Other) const = default;

  friend void
  writeToLog(Logger &L, const dla::TypeLinkTag &T, int /* Ignore */);

}; // end class TypeLinkTag

class LayoutTypeSystem;

enum InterferingChildrenInfo {
  Unknown = 0,
  AllChildrenAreInterfering,
  AllChildrenAreNonInterfering,
};

struct LayoutTypeSystemNode {
  const uint64_t ID = 0ULL;

  using Link = std::pair<LayoutTypeSystemNode *, const TypeLinkTag *>;

  struct NeighborLinkComparison {
    using is_transparent = std::true_type;

    struct Helper {
    private:
      uint64_t ID;
      const TypeLinkTag *const TagPointer;

    public:
      Helper() : ID(0), TagPointer(nullptr) {}
      Helper(const std::pair<uint64_t, const TypeLinkTag *> &Pair) :
        ID(Pair.first), TagPointer(Pair.second) {}
      Helper(const Link &Pair) : Helper({ Pair.first->ID, Pair.second }) {}
      Helper(const Helper &) = default;

      bool operator<(const Helper &Other) const {
        if (auto Cmp = ID <=> Other.ID; Cmp != 0)
          return Cmp < 0;

        if (nullptr == TagPointer or nullptr == Other.TagPointer)
          return TagPointer < Other.TagPointer;

        return *TagPointer < *Other.TagPointer;
      }
    };

    bool operator()(const Helper &&LHS, const Helper &&RHS) const {
      return LHS < RHS;
    }
  };

  /// What the links to a node name. A node's slot survives the node when a
  /// merge keeps the links of the node that goes away: the slot then changes
  /// owner, and the links to it need no rewriting.
  struct LinkSlot {
    LayoutTypeSystemNode *Owner = nullptr;
    /// The IDs of the earlier owners under which a link to this slot may still
    /// be positioned in a neighbor's link set
    llvm::SmallVector<uint64_t, 0> PastIDs;
  };

  /// The links of a node in one direction, ordered by the neighbor's ID and
  /// then by tag.
  ///
  /// An iterator yields a link as a value: how the links are stored belongs
  /// to LayoutTypeSystem, which alone changes them. An iterator stays valid
  /// until the link it designates is erased or moved.
  ///
  /// An entry names the neighbor's slot and keeps the ID it was positioned
  /// under, so the set is always ordered by keys that do not change in place.
  /// When a slot changes owner, the entries that name it are out of position
  /// until the set is next read: every read that depends on the order first
  /// repositions them, unless no slot has changed owner since the last time.
  class LinkSet {
  private:
    struct Entry {
      LinkSlot *Slot;
      const TypeLinkTag *Tag;
      uint64_t Position;
    };

    struct EntryComparison {
      using is_transparent = std::true_type;
      using Helper = NeighborLinkComparison::Helper;

      static Helper key(const Entry &E) {
        return Helper(
            std::pair<uint64_t, const TypeLinkTag *>(E.Position, E.Tag));
      }
      static Helper key(const Link &L) { return Helper(L); }
      static Helper key(const std::pair<uint64_t, const TypeLinkTag *> &P) {
        return Helper(P);
      }

      template <typename LHST, typename RHST>
      bool operator()(const LHST &LHS, const RHST &RHS) const {
        return key(LHS) < key(RHS);
      }
    };

    using Storage = std::set<Entry, EntryComparison>;
    using IDKey = std::pair<uint64_t, const TypeLinkTag *>;

    mutable Storage Links;
    /// The value of OwnerChanges when every entry was last in position
    mutable uint64_t CheckedAt = 0;
    /// How many times a slot has changed owner
    static inline uint64_t OwnerChanges = 0;

    friend class LayoutTypeSystem;

    static uint64_t currentID(const LinkSlot *Slot);

    /// Put every entry in the position of its slot's current owner
    void ensureOrdered() const {
      if (CheckedAt == OwnerChanges)
        return;
      llvm::SmallVector<Storage::iterator, 8> Stale;
      for (auto It = Links.begin(), End = Links.end(); It != End; ++It)
        if (It->Position != currentID(It->Slot))
          Stale.push_back(It);
      for (Storage::iterator It : Stale) {
        auto Handle = Links.extract(It);
        Handle.value().Position = currentID(Handle.value().Slot);
        bool Inserted = Links.insert(std::move(Handle)).inserted;
        revng_assert(Inserted);
      }
      CheckedAt = OwnerChanges;
    }

    /// The entry for \p Slot with \p Tag positioned under \p ID, if any
    Storage::iterator findAt(const LinkSlot *Slot, uint64_t ID,
                             const TypeLinkTag *Tag) const {
      auto It = Links.find(IDKey{ID, Tag});
      if (It != Links.end() and It->Slot == Slot)
        return It;
      return Links.end();
    }

    /// The entry for \p Slot with \p Tag, wherever it is positioned
    Storage::iterator findEntry(const LinkSlot *Slot,
                                const TypeLinkTag *Tag) const {
      auto It = findAt(Slot, currentID(Slot), Tag);
      for (auto ID = Slot->PastIDs.rbegin(), End = Slot->PastIDs.rend();
           It == Links.end() and ID != End; ++ID)
        It = findAt(Slot, *ID, Tag);
      return It;
    }

    /// Add a link to \p Slot with \p Tag, unless there is one
    bool insertEntry(LinkSlot *Slot, const TypeLinkTag *Tag) {
      if (findEntry(Slot, Tag) != Links.end())
        return false;
      bool Inserted = Links.insert(Entry{Slot, Tag, currentID(Slot)}).second;
      revng_assert(Inserted);
      return true;
    }

    bool eraseEntry(const LinkSlot *Slot, const TypeLinkTag *Tag) {
      auto It = findEntry(Slot, Tag);
      if (It == Links.end())
        return false;
      Links.erase(It);
      return true;
    }

    /// Erase the entries for \p Slot positioned under \p ID, with any tag
    void eraseEntriesAt(const LinkSlot *Slot, uint64_t ID) {
      auto It = Links.lower_bound(IDKey{ID, nullptr});
      auto End = Links.lower_bound(IDKey{ID + 1, nullptr});
      while (It != End) {
        if (It->Slot == Slot)
          It = Links.erase(It);
        else
          ++It;
      }
    }

    /// Erase the entries for \p Slot, with any tag
    void eraseEntriesOf(const LinkSlot *Slot) {
      eraseEntriesAt(Slot, currentID(Slot));
      for (uint64_t ID : Slot->PastIDs)
        eraseEntriesAt(Slot, ID);
    }

    /// Put the entry for \p Slot with \p Tag in the position of the slot's
    /// current owner
    void reposition(const LinkSlot *Slot, const TypeLinkTag *Tag) {
      auto It = findEntry(Slot, Tag);
      revng_assert(It != Links.end());
      if (It->Position == currentID(Slot))
        return;
      auto Handle = Links.extract(It);
      Handle.value().Position = currentID(Slot);
      bool Inserted = Links.insert(std::move(Handle)).inserted;
      revng_assert(Inserted);
    }

  public:
    using value_type = Link;

    class iterator {
    private:
      Storage::const_iterator It;
      friend class LinkSet;
      friend class LayoutTypeSystem;
      explicit iterator(Storage::const_iterator It) : It(It) {}

      struct Arrow {
        Link TheLink;
        const Link *operator->() const { return &TheLink; }
      };

    public:
      using iterator_category = std::bidirectional_iterator_tag;
      using value_type = Link;
      using difference_type = std::ptrdiff_t;
      using pointer = Arrow;
      using reference = const Link;

      iterator() = default;
      const Link operator*() const { return Link(It->Slot->Owner, It->Tag); }
      Arrow operator->() const { return Arrow{**this}; }
      iterator &operator++() {
        ++It;
        return *this;
      }
      iterator operator++(int) {
        iterator Old = *this;
        ++It;
        return Old;
      }
      iterator &operator--() {
        --It;
        return *this;
      }
      iterator operator--(int) {
        iterator Old = *this;
        --It;
        return Old;
      }
      bool operator==(const iterator &) const = default;
    };
    using const_iterator = iterator;

    iterator begin() const {
      ensureOrdered();
      return iterator(Links.begin());
    }
    iterator end() const {
      ensureOrdered();
      return iterator(Links.end());
    }
    size_t size() const { return Links.size(); }
    bool empty() const { return Links.empty(); }

    template <typename KeyT> iterator find(const KeyT &Key) const {
      ensureOrdered();
      return iterator(Links.find(Key));
    }
    template <typename KeyT> bool contains(const KeyT &Key) const {
      ensureOrdered();
      return Links.contains(Key);
    }
    bool contains(const Link &Key) const {
      ensureOrdered();
      return Links.contains(Key);
    }
    iterator find(const Link &Key) const {
      ensureOrdered();
      return iterator(Links.find(Key));
    }
    template <typename KeyT> iterator lower_bound(const KeyT &Key) const {
      ensureOrdered();
      return iterator(Links.lower_bound(Key));
    }
    template <typename KeyT> iterator upper_bound(const KeyT &Key) const {
      ensureOrdered();
      return iterator(Links.upper_bound(Key));
    }
  };

  using NeighborsSet = LinkSet;
  using NeighborIterator = LinkSet::iterator;
  const NeighborsSet &successors() const { return Successors; }
  const NeighborsSet &predecessors() const { return Predecessors; }

private:
  // Only LayoutTypeSystem changes the links, so that it can keep both
  // directions of each link consistent
  friend class LayoutTypeSystem;
  NeighborsSet Successors{};
  NeighborsSet Predecessors{};
  std::unique_ptr<LinkSlot> Slot{new LinkSlot{this, {}}};

public:
  uint64_t Size{};
  InterferingChildrenInfo InterferingInfo{ Unknown };
  bool NonScalar{ false };
  // A function argument or return whose own layout remains observable.
  bool IsTypeRoot{ false };
  // A pointer interpretation at offset zero whose pointee layout was pruned.
  // This does not add containment constraints to the layout graph.
  bool HasUnknownPointee{ false };

  LayoutTypeSystemNode(uint64_t I) : ID(I) {}

public:
  // This method should never be called, but it's necessary to be able to use
  // some llvm::GraphTraits algorithms, otherwise they wouldn't compile.
  LayoutTypeSystem *getParent() {
    revng_abort();
    return nullptr;
  }

  void print(llvm::raw_ostream &OS) const;

  void printAsOperand(llvm::raw_ostream &OS, bool /* unused */) const {
    print(OS);
  }
};

inline uint64_t LayoutTypeSystemNode::LinkSet::currentID(const LinkSlot *Slot) {
  return Slot->Owner->ID;
}

/// This class handles equivalence classes between indexes of vectors
class VectEqClasses : public llvm::IntEqClasses {
private:
  // ID of the first removed ID
  std::optional<unsigned> RemovedID = {};
  unsigned NElems = 0;

private:
  /// Used internally, operator[] is removed for this class
  unsigned lookupEqClass(unsigned ID) const {
    return llvm::IntEqClasses::operator[](ID);
  }

public:
  /// Add 1 element with its own equivalence class
  unsigned growBy1();

  /// Remove the whole equivalence class of \a ID
  void remove(const unsigned ID);

  /// Check if the element has been removed
  bool isRemoved(const unsigned ID) const;

  /// Get the total number of elements added
  unsigned getNumElements() const { return NElems; }

public:
  /// You can't access the Eq Classes directly, some might be deleted
  unsigned operator[](unsigned) const = delete;

  /// Get the Equivalence class ID of an element (must be compressed)
  ///\return empty if the element is out-of-bounds or has been removed
  std::optional<unsigned> getEqClassID(const unsigned ID) const;

  /// Get all the elements that are in the same equivalence class of \a ID
  ///\note Expensive: performs a linear scan of all the elements
  std::vector<unsigned> computeEqClass(const unsigned ID) const;

  /// Check if \a ID1 and \a ID2 have the same equivalence class
  bool haveSameEqClass(unsigned ID1, unsigned ID2) const;
};

/// This class is used to print debug information about the TypeSystem
///
/// Override this to obtain implementation-specific debug prints.
struct TSDebugPrinter {
  virtual void printNodeContent(const LayoutTypeSystem &TS,
                                const LayoutTypeSystemNode *N,
                                llvm::raw_fd_ostream &File) const;

  virtual ~TSDebugPrinter() {}
};

class LayoutTypeSystem {
public:
  using Node = LayoutTypeSystemNode;
  using NodePtr = LayoutTypeSystemNode *;
  using NodeUniquePtr = std::unique_ptr<LayoutTypeSystemNode>;
  using NeighborIterator = LayoutTypeSystemNode::NeighborIterator;

  /// Orders nodes by ID. Nodes are bump-allocated, so ordering them by address
  /// would make the iteration order depend on the allocator.
  struct NodeComparison {
    bool operator()(const LayoutTypeSystemNode *LHS,
                    const LayoutTypeSystemNode *RHS) const {
      return LHS->ID < RHS->ID;
    }
  };

  using NodeSet = std::set<LayoutTypeSystemNode *, NodeComparison>;

  LayoutTypeSystem() : DebugPrinter(new TSDebugPrinter) {}

  ~LayoutTypeSystem() {
    for (auto *Layout : Layouts) {
      Layout->~LayoutTypeSystemNode();
      NodeAllocator.Deallocate(Layout);
    }
    Layouts.clear();
  }

public:
  LayoutTypeSystemNode *createArtificialLayoutType();

  llvm::SmallVector<LayoutTypeSystemNode *, 2>
  createArtificialLayoutTypes(unsigned N);

protected:
  // This method is templated only to enable perfect forwarding.
  template<typename TagT>
  std::pair<const TypeLinkTag *, bool>
  addLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt, TagT &&Tag) {
    if (Src == nullptr or Tgt == nullptr or Src == Tgt)
      return std::make_pair(nullptr, false);
    revng_assert(Layouts.contains(Src));
    revng_assert(Layouts.contains(Tgt));
    auto It = LinkTags.insert(std::forward<TagT>(Tag)).first;
    revng_assert(It != LinkTags.end());
    const TypeLinkTag *T = &*It;
    bool New = Src->Successors.insertEntry(Tgt->Slot.get(), T);
    New |= Tgt->Predecessors.insertEntry(Src->Slot.get(), T);
    return std::make_pair(T, New);
  }

public:
  std::pair<const TypeLinkTag *, bool>
  addEqualityLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt) {
    auto ForwardLinkTag = addLink(Src, Tgt, dla::TypeLinkTag::equalityTag());
    auto BackwardLinkTag = addLink(Tgt, Src, dla::TypeLinkTag::equalityTag());
    revng_assert(ForwardLinkTag == BackwardLinkTag);
    return ForwardLinkTag;
  }

  // This method is templated just to enable perfect forwarding.
  template<typename OffsetExpressionT>
  std::pair<const TypeLinkTag *, bool>
  addInstanceLink(LayoutTypeSystemNode *Src,
                  LayoutTypeSystemNode *Tgt,
                  OffsetExpressionT &&OE) {
    using OET = OffsetExpressionT;

    // HACK: If the offset is greater than 64K, avoid adding the link. This
    // avoids creating gigantic structs with a big leading padding in cases in
    // which the DLA was not able to recognize a constant as the base address.
    // In the future, this should be handled through segments.
    if (OE.Offset > 0xFFFF)
      return std::make_pair(nullptr, false);

    return addLink(Src,
                   Tgt,
                   dla::TypeLinkTag::instanceTag(std::forward<OET>(OE)));
  }

  std::pair<const TypeLinkTag *, bool>
  addPointerLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt) {
    return addLink(Src, Tgt, dla::TypeLinkTag::pointerTag());
  }

  void dumpDotOnFile(const char *FName,
                     bool ShowCollapsed = false) const debug_function;

  void dumpDotOnFile(const std::string &FName,
                     bool ShowCollapsed = false) const {
    dumpDotOnFile(FName.c_str(), ShowCollapsed);
  }

  auto getNumLayouts() const { return Layouts.size(); }

  auto getLayoutsRange() const {
    return llvm::make_range(Layouts.begin(), Layouts.end());
  }

public:
  void mergeNodes(llvm::ArrayRef<LayoutTypeSystemNode *> ToMerge);

  void removeNode(LayoutTypeSystemNode *N);

  void moveEdgeTarget(LayoutTypeSystemNode *OldTgt,
                      LayoutTypeSystemNode *NewTgt,
                      NeighborIterator InverseEdgeIt,
                      int64_t OffsetToSum);

  void moveEdgeSource(LayoutTypeSystemNode *OldSrc,
                      LayoutTypeSystemNode *NewSrc,
                      NeighborIterator EdgeIt,
                      int64_t OffsetToSum);

  NeighborIterator eraseEdge(LayoutTypeSystemNode *Src,
                             NeighborIterator EdgeIt);

  void dropOutgoingEdges(LayoutTypeSystemNode *N);

private:
  static void fixPredSucc(LayoutTypeSystemNode *From,
                          LayoutTypeSystemNode *Into);

  static void moveEdgeTargetWithoutSumming(LayoutTypeSystemNode *OldTgt,
                                           LayoutTypeSystemNode *NewTgt,
                                           NeighborIterator InverseEdgeIt);

  static void moveEdgeSourceWithoutSumming(LayoutTypeSystemNode *OldSrc,
                                           LayoutTypeSystemNode *NewSrc,
                                           NeighborIterator EdgeIt);

private:
  uint64_t NID = 0ULL;

  // Holds all the LayoutTypeSystemNode
  llvm::BumpPtrAllocator NodeAllocator = {};
  NodeSet Layouts = {};

  // Holds the link tags, so that they can be deduplicated and referred to using
  // TypeLinkTag * in the links inside LayoutTypeSystemNode
  std::set<TypeLinkTag> LinkTags = {};

public:
  // Checks that is valid, and returns true if it is, false otherwise
  bool verifyConsistency() const;

  /// Abort unless every link set is as LinkSet describes: each entry names a
  /// live node, is positioned under an ID its slot has had, appears once, and
  /// has its counterpart in the neighbor; and once read, the set is in order.
  void checkLinks() const;
  // Checks that is valid and a DAG, and returns true if it is, false otherwise
  bool verifyDAG() const;
  // Checks that is valid and a DAG on instance. Returns true on success.
  bool verifyInstanceDAG() const;
  // Checks that is valid and a DAG on pointer. Returns true on success.
  bool verifyPointerDAG() const;
  // Checks that there are no leaf nodes without valid layout information
  bool verifyLeafs() const;
  // Checks that there are no equality edges.
  bool verifyNoEquality() const;
  // Checks that there are no equality edges.
  bool verifyInstanceAtOffset0DAG() const;
  // Checks that no union node has only one child
  bool verifyUnions() const;

private:
  // Equivalence classes between nodes. Each node is identified by an ID.
  VectEqClasses EqClasses;
  // Object that defines how the content of each node should be printed
  std::unique_ptr<TSDebugPrinter> DebugPrinter;

public:
  unsigned getNID() const { return NID; }

  VectEqClasses &getEqClasses() { return EqClasses; }
  const VectEqClasses &getEqClasses() const { return EqClasses; }

  void setDebugPrinter(std::unique_ptr<TSDebugPrinter> &&Printer) {
    DebugPrinter = std::move(Printer);
  }
}; // end class LayoutTypeSystem

} // end namespace dla

template<>
struct llvm::GraphTraits<dla::LayoutTypeSystemNode *> {
protected:
  using NodeT = dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->successors().begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->successors().end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->successors().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->successors().end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<const dla::LayoutTypeSystemNode *> {
protected:
  using NodeT = const dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->successors().begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->successors().end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->successors().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->successors().end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<llvm::Inverse<dla::LayoutTypeSystemNode *>> {
protected:
  using NodeT = dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->predecessors().begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->predecessors().end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->predecessors().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->predecessors().end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<llvm::Inverse<const dla::LayoutTypeSystemNode *>> {
protected:
  using NodeT = const dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->predecessors().begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->predecessors().end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->predecessors().begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->predecessors().end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<const dla::LayoutTypeSystem *>
  : public llvm::GraphTraits<const dla::LayoutTypeSystemNode *> {

public:
  using nodes_iterator = dla::LayoutTypeSystem::NodeSet::iterator;

  static NodeRef getEntryNode(const dla::LayoutTypeSystem *) { return nullptr; }

  static nodes_iterator nodes_begin(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().begin();
  }

  static nodes_iterator nodes_end(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().end();
  }

  static unsigned size(const dla::LayoutTypeSystem *G) {
    return G->getNumLayouts();
  }
}; // struct llvm::GraphTraits<dla::LayoutTypeSystem>

template<>
struct llvm::GraphTraits<dla::LayoutTypeSystem *>
  : public llvm::GraphTraits<dla::LayoutTypeSystemNode *> {

public:
  using nodes_iterator = dla::LayoutTypeSystem::NodeSet::iterator;

  static NodeRef getEntryNode(const dla::LayoutTypeSystem *) { return nullptr; }

  static nodes_iterator nodes_begin(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().begin();
  }

  static nodes_iterator nodes_end(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().end();
  }

  static unsigned size(dla::LayoutTypeSystem *G) { return G->getNumLayouts(); }
}; // struct llvm::GraphTraits<dla::LayoutTypeSystem>

namespace dla {

template<dla::TypeLinkTag::LinkKind K>
inline bool hasLinkKind(const dla::LayoutTypeSystemNode::Link &L) {
  if constexpr (K == dla::TypeLinkTag::LinkKind::LK_All)
    return true;
  else
    return L.second->getKind() == K;
}

template<dla::TypeLinkTag::LinkKind K>
inline bool hasNonPointerLinkKind(const dla::LayoutTypeSystemNode::Link &L) {
  static_assert(K != dla::TypeLinkTag::LinkKind::LK_Pointer);
  if constexpr (K == dla::TypeLinkTag::LinkKind::LK_All)
    return L.second->getKind() != dla::TypeLinkTag::LinkKind::LK_Pointer;
  else
    return L.second->getKind() == K;
}

inline bool
isEqualityEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Equality>(E);
}

inline bool
isInstanceEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Instance>(E);
}

inline bool
isInstanceOff0(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  if (not isInstanceEdge(E))
    return false;

  auto &OE = E.second->getOffsetExpr();
  return OE.Offset == 0 and OE.Strides.empty() and OE.TripCounts.empty();
}

inline bool
isStridedInstance(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  if (not isInstanceEdge(E))
    return false;

  auto &OE = E.second->getOffsetExpr();
  return not OE.Strides.empty();
}

inline bool
isNonStridedInstance(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef
                       &E) {
  return isInstanceEdge(E) and not isStridedInstance(E);
}

inline bool
isInstanceOffNon0(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return isInstanceEdge(E) and not isInstanceOff0(E);
}

inline bool
isPointerEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Pointer>(E);
}

inline bool
isNotPointerEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return not isPointerEdge(E);
}

inline bool isPointerNode(const LayoutTypeSystemNode *N) {
  return llvm::any_of(N->successors(), isPointerEdge);
}

inline bool isStructNode(const LayoutTypeSystemNode *N) {
  return N->InterferingInfo == AllChildrenAreNonInterfering and
         llvm::any_of(N->successors(), isNotPointerEdge);
}

inline bool isUnionNode(const LayoutTypeSystemNode *N) {
  return N->InterferingInfo == AllChildrenAreInterfering;
}

template<dla::TypeLinkTag::LinkKind K = dla::TypeLinkTag::LinkKind::LK_All>
inline bool isLeaf(const LayoutTypeSystemNode *N) {
  using LTSN = const LayoutTypeSystemNode;
  using GraphNodeT = LTSN *;
  using FilteredNodeT = EdgeFilteredGraph<GraphNodeT, hasNonPointerLinkKind<K>>;
  using GT = llvm::GraphTraits<FilteredNodeT>;
  return GT::child_begin(N) == GT::child_end(N);
}

inline bool isInstanceLeaf(const LayoutTypeSystemNode *N) {
  return isLeaf<dla::TypeLinkTag::LinkKind::LK_Instance>(N);
}

inline bool isPointerLeaf(const LayoutTypeSystemNode *N) {
  using LTSN = LayoutTypeSystemNode;
  using PointerNodeT = EdgeFilteredGraph<const LTSN *, isPointerEdge>;
  using PointerGraph = llvm::GraphTraits<PointerNodeT>;
  return PointerGraph::child_begin(N) == PointerGraph::child_end(N);
}

template<dla::TypeLinkTag::LinkKind K = dla::TypeLinkTag::LinkKind::LK_All>
inline bool isRoot(const LayoutTypeSystemNode *N) {
  using LTSN = const LayoutTypeSystemNode;
  using GraphNodeT = LTSN *;
  using FilteredNodeT = EdgeFilteredGraph<GraphNodeT, hasNonPointerLinkKind<K>>;
  using IGT = llvm::GraphTraits<llvm::Inverse<FilteredNodeT>>;
  return IGT::child_begin(N) == IGT::child_end(N);
}

inline bool isInstanceRoot(const LayoutTypeSystemNode *N) {
  return isRoot<dla::TypeLinkTag::LinkKind::LK_Instance>(N);
}

inline bool isPointerRoot(const LayoutTypeSystemNode *N) {
  using LTSN = LayoutTypeSystemNode;
  using PointerNodeT = EdgeFilteredGraph<const LTSN *, isPointerEdge>;
  using PointerGraph = llvm::GraphTraits<llvm::Inverse<PointerNodeT>>;
  return PointerGraph::child_begin(N) == PointerGraph::child_end(N);
}
} // end namespace dla
