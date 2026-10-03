//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"

#include "revng/ADT/GenericGraph.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Support/Debug.h"

#include "DLAStep.h"
#include "FieldSizeComputation.h"

static Logger Log{ "sort-accesses-hierarchically" };

namespace dla {

// Returns true if N has an incoming instance strided edge
static bool hasStridedParent(const LayoutTypeSystemNode *N) {
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isNotPointerEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  for (const auto &Edge : llvm::children_edges<InverseInstance>(N))
    if (isInstanceEdge(Edge)
        and not Edge.second->getOffsetExpr().Strides.empty())
      return true;
  return false;
}

// Returns true if N has an incoming instance edge from a node different from
// Parent
static bool hasAnotherParent(const LayoutTypeSystemNode *N,
                             const LayoutTypeSystemNode *Parent) {
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isNotPointerEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  for (const auto *P : llvm::children<InverseInstance>(N))
    if (P != Parent)
      return true;

  return false;
}

// Returns true if N has a pointer successor (predecessor if OnInverse is true)
template<bool OnInverse>
static bool hasPointerSuccessor(const LayoutTypeSystemNode *N) {
  using ConstPointerGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                              dla::isPointerEdge>;
  using Directed = std::conditional_t<OnInverse,
                                      llvm::Inverse<ConstPointerGraph>,
                                      ConstPointerGraph>;

  for ([[maybe_unused]] const auto *_ : llvm::children<Directed>(N))
    return true;

  return false;
}

// Returns true if N has an incoming pointer edge
static bool hasPointerParent(const LayoutTypeSystemNode *N) {
  return hasPointerSuccessor<true>(N);
}

// Returns true if N has an outgoing pointer edge
static bool hasPointerChild(const LayoutTypeSystemNode *N) {
  return hasPointerSuccessor<false>(N);
}

// Returns true if Child is an instance child of Parent that should not be
// destroyed by this dla::Step.
static bool isFixedChild(const LayoutTypeSystemNode *Parent,
                         const LayoutTypeSystemNode *Child) {

  // If is'a a leaf node it's fixed because it represents an access.
  // If Child has an outgoing or incoming pointer edge, then it's fixed.
  // If there's a strided edge going to Child, then Child is fixed.
  // If Child has another Parent that is different from Parent, then it's
  // fixed
  //
  // hasAnotherParent comes before the other edge checks, which visit every
  // outgoing or incoming edge of Child when it has no pointer or strided
  // edge: a child shared by many parents is fixed for each of them, and
  // usually its first incoming edge shows it.
  if (Child->NonScalar or isLeaf(Child) or hasAnotherParent(Child, Parent)
      or hasPointerChild(Child) or hasPointerParent(Child)
      or hasStridedParent(Child)) {
    return true;
  }

  // TODO: what if the Child has many incoming edges, and one comes
  // from Parent and the others all come from stuff that's between
  // Parent and Child? (e.g. P -> C1; P -> C2; C1 -> C2;) In the
  // current implementation C2 is considered fixed, because it has
  // another parent different from P.

  // TODO: what if the Child has both a in incoming strided edge and
  // a non-strided edge that come from Parent???
  //(e.g. P -offset-> C; P -strided-> C;)
  // In the current implementation C is considered fixed, because it
  // has an incoming strided edge.

  return false;
}

using NonPointerFilterT = EdgeFilteredGraph<dla::LayoutTypeSystemNode *,
                                            dla::isNotPointerEdge>;

static llvm::SetVector<LayoutTypeSystemNode *>
getVolatileChildren(LayoutTypeSystemNode *Parent) {
  llvm::SetVector<LayoutTypeSystemNode *> Result;
  for (auto *Child : llvm::children<NonPointerFilterT>(Parent))
    if (not isFixedChild(Parent, Child))
      Result.insert(Child);
  return Result;
}

using NeighborIterator = LayoutTypeSystem::NeighborIterator;

// This struct represents if an edge in LayoutTypeSystem can be pushed down
// another edge in LayoutTypeSystem, along with the resulting OffsetExpression
// if the push down takes place.
struct PushThroughComparisonResult {
  NeighborIterator ToPush;
  NeighborIterator Through;
  OffsetExpression OEAfterPush;
};

static OffsetExpression
computeOffsetAfterPush(NeighborIterator ToBePushed,
                       NeighborIterator ToBePushedThrough) {
  OffsetExpression Final;

  revng_assert(isInstanceEdge(*ToBePushed));
  revng_assert(isInstanceEdge(*ToBePushedThrough));

  const auto &ToPushOE = ToBePushed->second->getOffsetExpr();
  const auto &ThroughOE = ToBePushedThrough->second->getOffsetExpr();

  revng_assert(ToPushOE.Strides.empty() or ToPushOE.Strides.size() == 1);
  revng_assert(ThroughOE.Strides.empty() or ThroughOE.Strides.size() == 1);

  // The edge to push through always has the larger offset. Just subtract the
  // offset of the other edge.
  revng_assert(ToPushOE.Offset >= ThroughOE.Offset);
  Final.Offset = ToPushOE.Offset - ThroughOE.Offset;

  // If a strided edge is being pushed down another edge it means that the
  // whole array represented by the edge to push is contained inside the
  // element represented by the edge we're pushing it through (or a single
  // element of the array represented by the edge we're pushing it through, if
  // the edge we're pushing through is strided).
  // If the edge being pushed down is not strided, strides and trip counts are
  // empty.
  // So in all cases we can preserve Strides and TripCounts.
  Final.Strides = ToPushOE.Strides;
  Final.TripCounts = ToPushOE.TripCounts;

  // Pushing through a strided edge, we need to compute the reminder of the
  // offset inside the element of the array represented by the edge we're
  // pushing through.
  if (not ThroughOE.Strides.empty()) {
    revng_assert(ThroughOE.Strides.size() == 1);
    Final.Offset %= ThroughOE.Strides.front();
  }

  uint64_t ThroughElemSize = ToBePushedThrough->first->Size;
  uint64_t PushedFieldSize = getFieldSize(ToBePushed->first,
                                          ToBePushed->second);
  revng_assert(ThroughElemSize >= PushedFieldSize + Final.Offset);

  return Final;
}

// Returns true if adding an instance edge from Source to Target would close an
// instance loop.
static bool wouldCloseInstanceLoop(const LayoutTypeSystemNode *Parent,
                                   const LayoutTypeSystemNode *Source,
                                   const LayoutTypeSystemNode *Target) {
  // Self loops are never added in the first place.
  if (Source == Target)
    return false;

  // This function is expected to be called only in situations where Parent is
  // has instance edges going to both Source and Target.
  // For this reason, we do the reachability test on the Inverse. In this way we
  // we can use the Parent as a guard to keep the search quick and short.
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isInstanceEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  using LTSN = LayoutTypeSystemNode;
  using DFVisitSet = llvm::df_iterator_default_set<const LTSN *, 8>;

  DFVisitSet Visited;
  Visited.insert(Parent);
  for (const auto *Node :
       llvm::depth_first_ext(InverseInstance(Source), Visited))
    if (Node == Target)
      return true;

  return false;
}

// Compare the edges *AIt and *BIt to check if any of them can be pushed through
// the other. If so return proper info.
static std::optional<PushThroughComparisonResult>
canPushThrough(const LayoutTypeSystemNode *Parent,
               const NeighborIterator &AIt,
               const NeighborIterator &BIt) {
  revng_log(Log, "canPushThrough");
  LoggerIndent Indent{ Log };

  // An edge can never be pushed through itself
  if (AIt == BIt) {
    revng_log(Log, "nullopt: same");
    return std::nullopt;
  }

  // If any edge is not an instance edge, the edges are not comparable.
  if (not isInstanceEdge(*AIt) or not isInstanceEdge(*BIt)) {
    revng_log(Log, "nullopt: not instance");
    return std::nullopt;
  }

  // Here both edges are instance edges.

  const auto &[AChild, ATag] = *AIt;
  const auto &[BChild, BTag] = *BIt;

  if (isLeaf(AChild) and isLeaf(BChild)) {
    revng_log(Log, "nullopt: child");
    return std::nullopt;
  }

  const auto &[AOffset, AStrides, ATripCounts] = ATag->getOffsetExpr();
  const auto &[BOffset, BStrides, BTripCounts] = BTag->getOffsetExpr();

  revng_assert(AStrides.size() == ATripCounts.size());
  revng_assert(BStrides.size() == BTripCounts.size());
  revng_assert(AStrides.empty() or AStrides.size() == 1);
  revng_assert(BStrides.empty() or BStrides.size() == 1);

  uint64_t AFieldSize = getFieldSize(AChild, ATag);
  uint64_t BFieldSize = getFieldSize(BChild, BTag);

  uint64_t AFieldEnd = AOffset + AFieldSize;
  uint64_t BFieldEnd = BOffset + BFieldSize;

  // If A and B occupy disjoint ranges of memory, none of them can be pushed
  // through the other, so they are not comparable.
  if (AFieldEnd <= BOffset) {
    revng_log(Log, "nullopt: A ends before B starts");
    return std::nullopt;
  }

  if (BFieldEnd <= AOffset) {
    revng_log(Log, "nullopt: B ends before A starts");
    return std::nullopt;
  }

  // Here we have the guarantee that A and B occupy partly overlapping ranges.
  // They are not necessarily comparable yet, because they could still be partly
  // overlapping and not included.
  // Detect the partly overlapping case and return unordered for them
  if (AOffset < BOffset and AFieldEnd < BFieldEnd) {
    revng_log(Log, "nullopt: partly overlapping, A starts first");
    return std::nullopt;
  }
  if (AOffset > BOffset and AFieldEnd > BFieldEnd) {
    revng_log(Log, "nullopt: partly overlapping, B starts first");
    return std::nullopt;
  }

  // The two fields start at the same point and have the same size. None of them
  // strictly includes the other, so they need to be structurally inspected to
  // be merged. There's a separate pass doing this later in DLA, so we're not
  // taking care of it now.
  if (AOffset == BOffset and AFieldEnd == BFieldEnd) {
    revng_log(Log, "nullopt: equal ranges, needs structural inspection");
    return std::nullopt;
  }

  // Here we have the guarantee that one of the following holds:
  // - A is included in or occupies the same range as B
  // - B is included in or occupies the same range as A
  // Detect what is the case.
  bool AIsOuter = AFieldSize > BFieldSize;

  auto OuterIt = AIsOuter ? AIt : BIt;
  const auto &[Outer, OuterTag] = *OuterIt;

  // If the larger node is a leaf we cannot push the other down, so we bail out.
  if (isLeaf(Outer)) {
    revng_log(Log, "nullopt: outer is a leaf");
    return std::nullopt;
  }

  auto InnerIt = AIsOuter ? BIt : AIt;
  const auto &[Inner, InnerTag] = *InnerIt;

  const auto &InnerOffset = InnerTag->getOffsetExpr().Offset;
  const auto &[OuterOffset, OuterStrides, _] = OuterTag->getOffsetExpr();

  auto InnerFieldSize = AIsOuter ? BFieldSize : AFieldSize;
  auto OuterElemSize = OuterStrides.empty() ? Outer->Size :
                                              OuterStrides.front();

  // The inner fields starts at an higher offset (or equal) than the outer
  revng_assert(InnerOffset >= OuterOffset);
  auto OffsetAfterPush = InnerOffset - OuterOffset;

  auto OffsetInElem = OffsetAfterPush % OuterElemSize;
  auto EndByteInElem = OffsetInElem + InnerFieldSize;

  // When the inner field starts at the beginning of the outer and it has the
  // same size, it means that it spans the same exact bytes.
  // In principle there is nothing wrong with pushing inner through outer, but
  // that could close a instance loop, like in the following example if inner is
  // A->B and outer is A->C:
  //   A --> B --> C
  //    `----------^
  // So we detect this case and bail out.
  if (OffsetInElem == 0 and InnerFieldSize == Outer->Size
      and wouldCloseInstanceLoop(Parent, Outer, Inner)) {
    revng_log(Log,
              "nullopt: inner field covers a whole element of the "
              "outer, and pushing through would close an "
              "instance loop");
    return std::nullopt;
  }

  if (EndByteInElem > Outer->Size) {
    revng_log(Log,
              "nullopt: EndByteInElem: " << EndByteInElem << " > "
                                         << " Outer->Size: " << Outer->Size);
    return std::nullopt;
  }

  return PushThroughComparisonResult{
    .ToPush = InnerIt,
    .Through = OuterIt,
    .OEAfterPush = computeOffsetAfterPush(InnerIt, OuterIt)
  };
}

// Helper ordering for NeighborIterators. We need it here because we need to use
// such iterators and keys in associative containers.
// Notice that this might have undefined behaviour if dereferencing either LHS
// or RHS is undefined behaviour itself.
// The bottom line is that we should never insert invalid iterators into
// associative containers.
static std::weak_ordering operator<=>(const NeighborIterator &LHS,
                                      const NeighborIterator &RHS) {
  const auto &[LHSSucc, LHSTag] = *LHS;
  const auto &[RHSSucc, RHSTag] = *RHS;
  if (auto Cmp = LHSSucc->ID <=> RHSSucc->ID; Cmp != 0)
    return Cmp;

  if (nullptr == LHSTag or nullptr == RHSTag)
    return LHSTag <=> RHSTag;

  return *LHSTag <=> *RHSTag;
}

/// For each node that ArrangeAccessesHierarchically has not analyzed yet, the
/// number of its parents (through non-pointer edges) that it has not analyzed
/// yet. A node is ready to be analyzed when this count drops to 0. The step
/// tells it about every edge it adds or removes before a node is analyzed.
class PendingParents {
  llvm::DenseMap<const LayoutTypeSystemNode *, unsigned> Count;
  llvm::SmallVector<LayoutTypeSystemNode *> Released;

public:
  explicit PendingParents(LayoutTypeSystem &TS) {
    for (LayoutTypeSystemNode *N : llvm::nodes(&TS))
      Count[N] = 0;
    for (LayoutTypeSystemNode *N : llvm::nodes(&TS))
      for (LayoutTypeSystemNode *Child : distinctChildren(N))
        ++Count[Child];
  }

  bool isPending(const LayoutTypeSystemNode *N) const {
    return Count.count(N) != 0;
  }

  bool empty() const { return Count.empty(); }

  unsigned count(const LayoutTypeSystemNode *N) const {
    return Count.lookup(N);
  }

  /// The distinct children of \p N through non-pointer edges
  static llvm::SmallVector<LayoutTypeSystemNode *>
  distinctChildren(LayoutTypeSystemNode *N) {
    llvm::SmallVector<LayoutTypeSystemNode *> Result;
    for (LayoutTypeSystemNode *Child : llvm::children<NonPointerFilterT>(N))
      if (Result.empty() or Result.back() != Child)
        Result.push_back(Child);
    return Result;
  }

  /// Whether a non-pointer edge goes from \p Parent to \p Child
  static bool isParent(const LayoutTypeSystemNode *Parent,
                       const LayoutTypeSystemNode *Child) {
    using IDBasedKey = std::pair<uint64_t, const TypeLinkTag *>;
    auto &Predecessors = Child->Predecessors;
    auto It = Predecessors.lower_bound(IDBasedKey{ Parent->ID, nullptr });
    auto End = Predecessors.upper_bound(IDBasedKey{ Parent->ID + 1,
                                                    nullptr });
    for (; It != End; ++It)
      if (isNotPointerEdge(*It))
        return true;
    return false;
  }

  /// Call before adding a non-pointer edge from \p Parent to \p Child (the
  /// type system adds no edge from a node to itself)
  void beforeLink(const LayoutTypeSystemNode *Parent,
                  const LayoutTypeSystemNode *Child) {
    if (Parent != Child and isPending(Parent) and not isParent(Parent, Child))
      ++Count[Child];
  }

  /// Call after removing a non-pointer edge from \p Parent to \p Child
  void afterUnlink(const LayoutTypeSystemNode *Parent,
                   LayoutTypeSystemNode *Child) {
    if (isPending(Parent) and not isParent(Parent, Child))
      release(Child);
  }

  /// Call when \p N leaves the graph without being analyzed
  void erase(const LayoutTypeSystemNode *N) { Count.erase(N); }

  /// Call once \p N has been analyzed
  void analyzed(LayoutTypeSystemNode *N) {
    revng_assert(Count.lookup(N) == 0);
    Count.erase(N);
    for (LayoutTypeSystemNode *Child : distinctChildren(N))
      release(Child);
  }

  /// The nodes whose count dropped to 0 since the last call
  llvm::SmallVector<LayoutTypeSystemNode *> takeReady() {
    llvm::SmallVector<LayoutTypeSystemNode *> Result;
    for (LayoutTypeSystemNode *N : Released)
      if (isPending(N) and Count.lookup(N) == 0)
        Result.push_back(N);
    Released.clear();
    return Result;
  }

private:
  void release(LayoutTypeSystemNode *N) {
    auto It = Count.find(N);
    revng_assert(It != Count.end() and It->second > 0);
    if (--It->second == 0)
      Released.push_back(N);
  }
};

static llvm::SmallPtrSet<LayoutTypeSystemNode *, 8>
absorbVolatileChildren(LayoutTypeSystem &TS,
                       LayoutTypeSystemNode *Parent,
                       PendingParents *Pending = nullptr) {
  revng_log(Log, "absorbVolatileChildren of: " << Parent->ID);
  LoggerIndent Indent{ Log };

  llvm::SmallPtrSet<LayoutTypeSystemNode *, 8> Absorbed;

  if (isLeaf(Parent))
    return Absorbed;

  revng_assert(not hasPointerChild(Parent));

  for (auto VolatileChildren = getVolatileChildren(Parent);
       not VolatileChildren.empty();
       VolatileChildren = getVolatileChildren(Parent)) {

    struct InstanceEdge {
      OffsetExpression OE;
      LayoutTypeSystemNode *Target = nullptr;

      // Ordering and comparison
      std::strong_ordering operator<=>(const InstanceEdge &) const = default;
      bool operator==(const InstanceEdge &) const = default;
    };

    llvm::SetVector<InstanceEdge,
                    llvm::SmallVector<InstanceEdge, 16>,
                    llvm::SmallSet<InstanceEdge, 16>>
      CompoundEdges;

    for (const auto &[Child, Tag] :
         llvm::children_edges<NonPointerFilterT>(Parent)) {
      // Don't mess up fixed children
      if (not VolatileChildren.contains(Child))
        continue;

      OffsetExpression OE = Tag->getOffsetExpr();

      // At this point we know that Child doesn't have incoming or
      // outgoing pointer edges. So all the edges involving Child are
      // instance edges.
      for (const auto &[GrandChild, ChildTag] :
           llvm::children_edges<NonPointerFilterT>(Child)) {
        revng_assert(not VolatileChildren.contains(GrandChild));
        auto New = InstanceEdge{
          OffsetExpression::append(OE, ChildTag->getOffsetExpr()), GrandChild
        };
        CompoundEdges.insert(New);
      }
    }

    // Remove all volatile nodes
    for (LayoutTypeSystemNode *Volatile : VolatileChildren) {
      Absorbed.insert(Volatile);
      auto GrandChildren = PendingParents::distinctChildren(Volatile);
      TS.dropOutgoingEdges(Volatile);
      if (Pending)
        for (LayoutTypeSystemNode *GrandChild : GrandChildren)
          Pending->afterUnlink(Volatile, GrandChild);
      revng_log(Log, "Merge volatile child: " << Volatile->ID);
      TS.mergeNodes({ Parent, Volatile });
      if (Pending)
        Pending->erase(Volatile);
    }

    for (auto &[OffsetExpr, Target] : CompoundEdges.takeVector()) {
      if (Pending)
        Pending->beforeLink(Parent, Target);
      TS.addInstanceLink(Parent, Target, std::move(OffsetExpr));
    }
  }
  return Absorbed;
}

static void absorbVolatileChildren(LayoutTypeSystem &TS) {
  std::set<const dla::LayoutTypeSystemNode *> Visited;
  for (LayoutTypeSystemNode *Root : llvm::nodes(&TS)) {

    if (Visited.contains(Root))
      continue;

    if (not isRoot(Root))
      continue;

    for (LayoutTypeSystemNode *Node :
         llvm::post_order_ext(NonPointerFilterT(Root), Visited))
      absorbVolatileChildren(TS, Node);
  }
}

/// Byte ranges of fields, each with a number, to find the ones that overlap
/// a given range in time proportional to their count
class OverlapIndex {
  struct Field {
    uint64_t Start = 0;
    uint64_t End = 0;
    unsigned Number = 0;
  };

  // Sorted by start
  std::vector<Field> Fields;
  // The largest end in each range of Fields, as a segment tree
  std::vector<uint64_t> MaxEnd;
  size_t Leaves = 1;

public:
  void add(uint64_t Start, uint64_t End, unsigned Number) {
    revng_assert(Start < End);
    Fields.push_back({ Start, End, Number });
  }

  void build() {
    llvm::stable_sort(Fields, [](const Field &LHS, const Field &RHS) {
      return LHS.Start < RHS.Start;
    });
    while (Leaves < Fields.size())
      Leaves *= 2;
    MaxEnd.assign(2 * Leaves, 0);
    for (size_t I = 0; I < Fields.size(); ++I)
      MaxEnd[Leaves + I] = Fields[I].End;
    for (size_t I = Leaves - 1; I > 0; --I)
      MaxEnd[I] = std::max(MaxEnd[2 * I], MaxEnd[2 * I + 1]);
  }

  /// Call \p Callback with the number of each field that shares a byte with
  /// [Start, End)
  template<typename CallbackT>
  void forEachOverlapping(uint64_t Start,
                          uint64_t End,
                          CallbackT &&Callback) const {
    auto ByStart = [](const Field &F, uint64_t Offset) {
      return F.Start < Offset;
    };
    auto First = std::lower_bound(Fields.begin(),
                                  Fields.end(),
                                  Start,
                                  ByStart);

    // The fields starting inside the range
    for (auto It = First; It != Fields.end() and It->Start < End; ++It)
      Callback(It->Number);

    // The fields starting before it and ending inside or after it
    size_t Before = First - Fields.begin();
    llvm::SmallVector<std::tuple<size_t, size_t, size_t>, 32> Stack{
      { 1, 0, Leaves }
    };
    while (not Stack.empty()) {
      auto [Node, Low, High] = Stack.pop_back_val();
      if (Low >= Before or MaxEnd[Node] <= Start)
        continue;
      if (High - Low == 1) {
        Callback(Fields[Low].Number);
        continue;
      }
      size_t Middle = (Low + High) / 2;
      Stack.push_back({ 2 * Node + 1, Middle, High });
      Stack.push_back({ 2 * Node, Low, Middle });
    }
  }
};

bool ArrangeAccessesHierarchically::runOnTypeSystem(LayoutTypeSystem &TS) {
  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  bool Changed = false;

  absorbVolatileChildren(TS);

  // Analyze each node once, after all the nodes that have an instance edge to
  // it. Analyzing a node only adds or removes edges between it and the nodes
  // it reaches, so once every parent of a node has been analyzed its edges
  // stop changing, and analyzing it then arranges all of them at once. The
  // ready nodes are taken in reverse post order of the graph as it is when the
  // step starts, which is a topological order of that graph.

  // Add a fake root to be able to compute a RPOT from that.
  auto *FakeRoot = TS.createArtificialLayoutType();
  for (LayoutTypeSystemNode *Root : llvm::nodes(&TS)) {
    revng_assert(Root != nullptr);
    if (not isRoot(Root))
      continue;

    TS.addInstanceLink(FakeRoot, Root, OffsetExpression{});
  }

  // Number the nodes in RPOT
  llvm::DenseMap<const LayoutTypeSystemNode *, unsigned> Position;
  for (LayoutTypeSystemNode *Node :
       llvm::ReversePostOrderTraversal(NonPointerFilterT(FakeRoot)))
    Position.insert({ Node, Position.size() });

  // The fake root is never to analyze and can already be removed.
  Position.erase(FakeRoot);
  TS.removeNode(FakeRoot);
  revng_assert(Position.size() == TS.getNumLayouts());

  PendingParents Pending(TS);
  using ReadyEntry = std::pair<unsigned, LayoutTypeSystemNode *>;
  std::priority_queue<ReadyEntry, std::vector<ReadyEntry>, std::greater<>>
    Ready;
  for (LayoutTypeSystemNode *Node : llvm::nodes(&TS))
    if (Pending.count(Node) == 0)
      Ready.push({ Position.lookup(Node), Node });

  while (not Ready.empty()) {
    LayoutTypeSystemNode *Parent = Ready.top().second;
    Ready.pop();

    // A node absorbed into its parent leaves the graph unanalyzed
    if (not Pending.isPending(Parent))
      continue;

    {
      revng_log(Log, "Analyzing parent: " << Parent->ID);
      LoggerIndent Indent{ Log };

      // If we reach this point we have to analyze Parent, to see if some of its
      // Instance children can be pushed down some other of its Instance
      // children

      // We want to look at all instance children edges of Parent, and compute a
      // partial ordering between them, with the relationship "A should be
      // pushed down B".

      // Before doing this we want to absorb potential other nodes that became
      // volatile when pushing them down.
      // For instance if we had A->B, A->C, B->C, and C was pushed down B, then
      // C becomes volatile inside B.
      {
        absorbVolatileChildren(TS, Parent, &Pending);
      }

      // What is pushed through an edge E, in the order it arrives: single
      // edges, and everything pushed through another edge A before A itself
      // was pushed through E. Every edge that ends up pushed through E gets
      // the offset computeOffsetAfterPush gives for the two edges, and pushing
      // an edge a second time would change nothing: the edge it adds is
      // already there, or was skipped because it would close a loop, which it
      // still would since edges are only added meanwhile. So, instead of
      // copying what was pushed through A into each edge containing A, which
      // costs the depth of the hierarchy times the size of the lists, each
      // list refers to the lists it includes, and is expanded once, keeping
      // the first occurrence of each edge, when it is applied.
      struct PushedThrough {
        struct Event {
          // Pushed edge, when Included is null
          NeighborIterator Edge;
          // Everything pushed through another edge, when not null
          const PushedThrough *Included = nullptr;
        };
        llvm::SmallVector<Event> Events;

        // The pushed edges, each once, in the order they first arrived
        llvm::SmallVector<NeighborIterator> expand() const {
          llvm::SmallVector<NeighborIterator> Result;
          llvm::DenseSet<const LayoutTypeSystemNode::Link *> Seen;
          llvm::DenseSet<const PushedThrough *> Expanded{ this };
          llvm::SmallVector<std::pair<const PushedThrough *, size_t>> Stack{
            { this, 0 }
          };
          while (not Stack.empty()) {
            auto [List, Index] = Stack.back();
            if (Index == List->Events.size()) {
              Stack.pop_back();
              continue;
            }
            ++Stack.back().second;
            const Event &Next = List->Events[Index];
            if (Next.Included != nullptr) {
              // A list expanded before adds no edge that is not there yet
              if (Expanded.insert(Next.Included).second)
                Stack.push_back({ Next.Included, 0 });
            } else if (Seen.insert(&*Next.Edge).second) {
              Result.push_back(Next.Edge);
            }
          }
          return Result;
        }
      };

      // Owns the lists, which outlive their entries in ChildrenHierarchy when
      // other lists include them
      std::deque<PushedThrough> Lists;

      revng_log(Log, "Initialize ChildrenHierarchy");
      // A map whose key is an edge iterator representing an instance edge, and
      // the mapped type is a vector of other edges that can be pushed through
      // the key, along the updated offsets that they will have after pushing
      // them through it.
      // In particular, we only keep in this map the neighbor iterator that are
      // the roots of the hierarchy of instance edges, i.e. the instance edges
      // that cannot be pushed through any other edge.
      std::map<NeighborIterator, PushedThrough *> ChildrenHierarchy;

      using GT = llvm::GraphTraits<LayoutTypeSystemNode *>;

      // Initialize the ChildrenHierarchy.
      // At the beginning, all children edges are roots, and none of them has
      // other edges that can be pushed through them.
      for (auto AIt = GT::child_edge_begin(Parent),
                ChildEnd = GT::child_edge_end(Parent);
           AIt != ChildEnd;
           ++AIt) {
        if (isInstanceEdge(*AIt))
          ChildrenHierarchy[AIt] = &Lists.emplace_back();
      }

      // canPushThrough finds nothing for two edges whose fields share no
      // byte, and finding nothing changes nothing. So each root is compared
      // only with the later roots whose fields overlap its own, which an
      // index of the fields by byte range finds, in the order of
      // ChildrenHierarchy: the same comparisons that find something, in the
      // same order, without comparing all pairs of the children of a node
      // with thousands of them. A child of size 0 has no field size, and its
      // edges are compared with every root, as before.
      std::vector<NeighborIterator> Roots;
      llvm::DenseMap<const LayoutTypeSystemNode::Link *, unsigned> RootNumber;
      std::vector<bool> IsRoot;
      OverlapIndex Overlaps;
      llvm::SmallVector<unsigned> Unsized;
      for (const auto &[Edge, Unused] : ChildrenHierarchy) {
        unsigned Number = Roots.size();
        Roots.push_back(Edge);
        RootNumber[&*Edge] = Number;
        IsRoot.push_back(true);
        if (Edge->first->Size == 0) {
          Unsized.push_back(Number);
        } else {
          uint64_t Start = Edge->second->getOffsetExpr().Offset;
          Overlaps.add(Start,
                       Start + getFieldSize(Edge->first, Edge->second),
                       Number);
        }
      }
      Overlaps.build();

      // The roots after Number in ChildrenHierarchy that canPushThrough may
      // find something with, in that order
      auto Candidates = [&](unsigned Number) {
        llvm::SmallVector<unsigned> Result;
        auto Add = [&](unsigned Other) {
          if (Other > Number and IsRoot[Other])
            Result.push_back(Other);
        };
        NeighborIterator Edge = Roots[Number];
        if (Edge->first->Size == 0) {
          for (unsigned Other = Number + 1; Other < Roots.size(); ++Other)
            Add(Other);
          return Result;
        }
        uint64_t Start = Edge->second->getOffsetExpr().Offset;
        uint64_t End = Start + getFieldSize(Edge->first, Edge->second);
        Overlaps.forEachOverlapping(Start, End, Add);
        for (unsigned Other : Unsized)
          Add(Other);
        llvm::sort(Result);
        return Result;
      };

      revng_log(Log, "Compare children edges");
      // Now compare each root only with other roots
      auto ARootIt = ChildrenHierarchy.begin();
      auto ARootNext = ARootIt;
      auto HierarchyEnd = ChildrenHierarchy.end();
      for (; ARootIt != HierarchyEnd; ARootIt = ARootNext) {
        LoggerIndent ChildIndent{ Log };

        auto &[AEdgeIt, PushedInsideA] = *ARootIt;

        revng_log(Log,
                  "comparing AEdge: " << AEdgeIt->first->ID
                                      << " label: " << AEdgeIt->second);

        // A vector of instance edges that are contained in AEdgeIt and that
        // should be pushed through it.
        llvm::SmallVector<NeighborIterator> ContainedInA;

        // A vector of instance edges that contain AEdgeIt. AEdgeIt will have to
        // be pushed through all of them.
        llvm::SmallVector<NeighborIterator> ContainsA;

        for (unsigned BNumber : Candidates(RootNumber.lookup(&*AEdgeIt))) {
          LoggerIndent OtherChildIndent{ Log };

          NeighborIterator BEdgeIt = Roots[BNumber];
          revng_log(Log,
                    "with BEdge: " << BEdgeIt->first->ID
                                   << " label: " << BEdgeIt->second);

          auto MaybePushThrough = canPushThrough(Parent, AEdgeIt, BEdgeIt);
          if (not MaybePushThrough.has_value())
            continue;

          auto &[ToPush, Through, OEAfterPush] = MaybePushThrough.value();
          revng_assert(ToPush == AEdgeIt or ToPush == BEdgeIt);
          revng_assert(Through == AEdgeIt or Through == BEdgeIt);
          revng_assert(Through != ToPush);
          if (ToPush == AEdgeIt) {
            revng_assert(Through == BEdgeIt);
            // AEdgeIt can be pushed inside BEdgeIt
            ContainsA.push_back(BEdgeIt);
          } else {
            revng_assert(ToPush == BEdgeIt and Through == AEdgeIt);
            // BEdgeIt can be pushed inside AEdgeIt
            ContainedInA.push_back(BEdgeIt);
          }
        }

        // Now, push all the roots contained in A into A, and remove them from
        // ChildrenHierarchy, since they're not roots anymore.
        for (NeighborIterator PushedInA : ContainedInA) {
          ChildrenHierarchy.erase(PushedInA);
          IsRoot[RootNumber.lookup(&*PushedInA)] = false;
          PushedInsideA->Events.push_back({ PushedInA });
        }

        // Then, for all the other edges that contain A, A can be pushed through
        // them, along with all the nodes that have already been pushed through
        // A itself (that transitively can be pushed through the others too).
        // After we've pushed A through all edges in ContainsA, we can remove A
        // itelf from ChildrenHierarchy, since it's not a root anymore.
        ARootNext = std::next(ARootIt);
        if (not ContainsA.empty()) {
          for (NeighborIterator LargerThanA : ContainsA) {
            // A can be pushed through LargerThanA, and so can all the things
            // that were previously pushed through A.
            PushedThrough *PushedThroughLargerThanA = ChildrenHierarchy.at(
              LargerThanA);
            PushedThroughLargerThanA->Events.push_back({ {}, PushedInsideA });
            PushedThroughLargerThanA->Events.push_back({ AEdgeIt });
          }
          // Now AEdgeIt is not a root anymore, so we have to erase it and
          // update ARootNext.
          IsRoot[RootNumber.lookup(&*AEdgeIt)] = false;
          ARootNext = ChildrenHierarchy.erase(ARootIt);
        }
      }

      llvm::SmallSet<NeighborIterator, 4> EdgesToErase;
      {
        revng_log(Log, "Collect EdgesToErase");
        LoggerIndent IndentCollect{ Log };
        for (auto &[EdgeToPushThrough, ToPush] : ChildrenHierarchy) {
          llvm::SmallVector<NeighborIterator> EdgesToPush = ToPush->expand();

          if (EdgesToPush.empty())
            continue;

          auto *ToPushThrough = EdgeToPushThrough->first;
          revng_assert(Pending.isPending(ToPushThrough));

          for (NeighborIterator PushedEdgeIt : EdgesToPush) {
            auto *ToPushDown = PushedEdgeIt->first;
            OffsetExpression FinalOE = computeOffsetAfterPush(PushedEdgeIt,
                                                              EdgeToPushThrough);

            // We already checked that we don't close an instance loop, but we
            // did it on the graph before any push happened, and edges were only
            // compared pairwise.
            // Two individual pushes, each individually harmless, can still
            // close a loop when they are both applied.
            // This check is meant to detect this case.
            //
            // A loop is only closed if ToPushDown already reaches
            // ToPushThrough via instance edges, which means ToPushThrough is
            // not larger than ToPushDown itself. But at the same time, the fact
            // that ToPushDown is being pushed down ToPushThrough means that the
            // size of the pushed field will not be larger than ToPushThrough.
            // This can only happen when the field that is being pushed down
            // will end up covering the whole ToPushThrough.
            // This is cheap to check and more rarely requires a visit.
            uint64_t PushedFieldSize = getFieldSize(ToPushDown,
                                                    PushedEdgeIt->second);
            if (FinalOE.Offset == 0 and PushedFieldSize == ToPushThrough->Size
                and wouldCloseInstanceLoop(Parent, ToPushThrough, ToPushDown)) {
              revng_log(Log,
                        "skip: " << ToPushThrough->ID << " -> "
                                 << ToPushDown->ID
                                 << " would close an instance loop");
              continue;
            }

            // An edge can be pused through many edges. It is enough that we
            // manage to push it through one to decide that we want to get rid
            // of the original edge, because in the very moment we push it
            // through at least one other edge we are preserving the information
            // it represents, just reshaping it into another form.
            EdgesToErase.insert(PushedEdgeIt);

            revng_assert(not isLeaf(ToPushThrough));
            Pending.beforeLink(ToPushThrough, ToPushDown);
            TS.addInstanceLink(ToPushThrough, ToPushDown, std::move(FinalOE));
          }
        }
      }

      // Now finally clean up the edges that were pushed down.
      {
        revng_log(Log, "Erase edges");
        LoggerIndent IndentErase{ Log };
        for (const auto &ToErase : EdgesToErase) {
          revng_log(Log,
                    "erase: " << Parent->ID << " -> " << ToErase->first->ID
                              << " label: " << ToErase->second);
          LayoutTypeSystemNode *Child = ToErase->first;
          TS.eraseEdge(Parent, ToErase);
          Pending.afterUnlink(Parent, Child);
        }
      }

      Changed |= not EdgesToErase.empty();
    }

    Pending.analyzed(Parent);
    for (LayoutTypeSystemNode *Node : Pending.takeReady())
      Ready.push({ Position.lookup(Node), Node });
  }
  revng_assert(Pending.empty());

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  return Changed;
}

} // end namespace dla
