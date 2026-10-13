//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <set>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/ADT/GenericGraph.h"
#include "revng/ADT/SmallMap.h"

#include "DLAStep.h"
#include "FieldSizeComputation.h"

namespace dla {

struct ChildrenKey {
  uint64_t Size;
  const TypeLinkTag *Tag = nullptr;
  bool operator==(const ChildrenKey &) const noexcept = default;
  std::strong_ordering
  operator<=>(const ChildrenKey &) const noexcept = default;
};

using NodePredicate = const std::function<bool(const LayoutTypeSystemNode *)>;

using NeighborIterator = LayoutTypeSystemNode::NeighborIterator;

// Order neighbors as the type system does, by child ID and then by tag. The
// order decides which of two incomparable pointer children resolveUnion keeps,
// so it must not depend on where the nodes and tags were allocated.
static bool neighborLess(const NeighborIterator &AIt,
                         const NeighborIterator &BIt) {
  using Key = LayoutTypeSystemNode::NeighborLinkComparison::Helper;
  return Key(*AIt) < Key(*BIt);
}

using NeighborLess = std::integral_constant<decltype(&neighborLess),
                                            neighborLess>;

using NeighborSet = std::set<NeighborIterator, NeighborLess>;

static SmallMap<ChildrenKey, NeighborSet, 8>
getOverlappingLeafChildren(LayoutTypeSystemNode *N) {

  SmallMap<ChildrenKey, NeighborSet, 8> Result;

  auto ChildIt = N->successors().begin();
  auto ChildEnd = N->successors().end();
  for (; ChildIt != ChildEnd; ++ChildIt) {
    if (not isInstanceEdge(*ChildIt))
      continue;
    auto &[Child, Tag] = *ChildIt;
    if (not isLeaf(Child))
      continue;
    Result[ChildrenKey{ getFieldSize(Child, Tag), Tag }].insert(ChildIt);
  }

  return Result;
}

using GT = llvm::GraphTraits<LayoutTypeSystemNode *>;

static bool isInstanceAtOffset0(const GT::EdgeRef &E) {
  if (not isInstanceEdge(E))
    return false;

  return not E.second->getOffsetExpr().Offset;
}

using CInstance0 =
    EdgeFilteredGraph<const dla::LayoutTypeSystemNode *, isInstanceAtOffset0>;
using ConstNodeSet =
    llvm::df_iterator_default_set<const LayoutTypeSystemNode *, 8>;

// The nodes whose types transitively contain the type represented by N at
// offset 0, N included
static ConstNodeSet getContainersAtOffset0(const LayoutTypeSystemNode *N) {
  ConstNodeSet Result;
  for ([[maybe_unused]] const LayoutTypeSystemNode *_ :
       llvm::depth_first_ext(llvm::Inverse<CInstance0>(N), Result))
    ;
  return Result;
}

static const LayoutTypeSystemNode *getPointee(const LayoutTypeSystemNode *N) {
  if (not isPointerNode(N))
    return nullptr;
  revng_assert(N->successors().size() == 1);
  return N->successors().begin()->first;
}

// Whenever in ChildrenSet there are multiple pointer children that point to
// types that are one at offset 0 of the other, it removes from Parent the
// pointer children that point to the less-general types.
static bool resolveUnion(LayoutTypeSystem &TS,
                         LayoutTypeSystemNode *Parent,
                         NeighborSet &ChildrenSet) {
  bool Changed = false;

  // Two children are comparable only if both are pointers: pointer use does
  // not disprove a scalar interpretation of the same bytes. A pointer child
  // is removed when a child before it in ChildrenSet points to a type that
  // transitively contains its pointee at offset 0. A child is never removed
  // for a child after it.
  //
  // The question is asked from the pointee that would be contained: the types
  // that contain a type at offset 0 are few, while the types a type contains
  // can be most of the graph. The containers of a pointee are the same for
  // every child that is compared with it. Removing a child of Parent takes
  // Parent out of the containers of that child alone, since a leaf contains
  // nothing.
  llvm::DenseMap<const LayoutTypeSystemNode *, ConstNodeSet> Containers;
  for (auto AIt = ChildrenSet.begin(); AIt != ChildrenSet.end(); ++AIt) {
    const LayoutTypeSystemNode *APointee = getPointee((*AIt)->first);
    if (APointee == nullptr)
      continue;

    for (auto BIt = std::next(AIt); BIt != ChildrenSet.end();) {
      auto BChildIt = *BIt;
      const LayoutTypeSystemNode *BChild = BChildIt->first;
      const LayoutTypeSystemNode *BPointee = getPointee(BChild);
      if (BPointee == nullptr or BPointee == APointee) {
        ++BIt;
        continue;
      }

      auto It = Containers.find(BPointee);
      if (It == Containers.end()) {
        ConstNodeSet Found = getContainersAtOffset0(BPointee);
        It = Containers.try_emplace(BPointee, std::move(Found)).first;
      }

      if (not It->second.contains(APointee)) {
        ++BIt;
        continue;
      }

      // A can reach more types on the DLA graph than B.
      // Remove B.
      BIt = ChildrenSet.erase(BIt);
      TS.eraseEdge(Parent, BChildIt);
      Changed = true;
      Containers.erase(BChild);
    }
  }
  // TODO: should we remove some nodes altogether? Or should we merge the
  // nodes that are killed with the nodes that survive?
  return Changed;
}

bool ResolveLeafUnions::runOnTypeSystem(LayoutTypeSystem &TS) {
  bool Changed = false;

  for (LayoutTypeSystemNode *Node : llvm::nodes(&TS)) {
    auto LeafChildrenSets = getOverlappingLeafChildren(Node);
    for (auto &LeafChildrenSet : llvm::make_second_range(LeafChildrenSets))
      Changed |= resolveUnion(TS, Node, LeafChildrenSet);
  }

  return Changed;
}

} // end namespace dla
