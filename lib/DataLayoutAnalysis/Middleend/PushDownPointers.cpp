//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"

#include "revng/ADT/FilteredGraphTraits.h"

#include "DLAStep.h"

namespace dla {

using GT = llvm::GraphTraits<LayoutTypeSystemNode *>;

static bool isInstanceAtOffset0(const GT::EdgeRef &E) {
  if (not isInstanceEdge(E))
    return false;

  return not E.second->getOffsetExpr().Offset;
}

using Instance0Graph = EdgeFilteredGraph<dla::LayoutTypeSystemNode *,
                                         isInstanceAtOffset0>;

using CInstance0Graph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          isInstanceAtOffset0>;

using Instance0Inverse = llvm::Inverse<Instance0Graph>;

using CInstance0Inverse = llvm::Inverse<CInstance0Graph>;

using DFVisitSet = llvm::df_iterator_default_set<const LayoutTypeSystemNode *,
                                                 8>;

static DFVisitSet getGrandParentsAtOffset0(const LayoutTypeSystemNode *N) {

  DFVisitSet Result;
  for ([[maybe_unused]] const auto *_ :
       llvm::depth_first_ext(CInstance0Inverse(N), Result))
    ;
  return Result;
}

static DFVisitSet getChildrenAtOffset0(const LayoutTypeSystemNode *Parent) {
  DFVisitSet Children;
  const auto &Instance0SuccessorsRange =
      llvm::make_filter_range(Parent->successors(), isInstanceAtOffset0);
  for (const LayoutTypeSystemNode *Child :
       llvm::make_first_range(Instance0SuccessorsRange))
    Children.insert(Child);
  return Children;
}

static uint64_t getNumChildrenAtOffset0(const LayoutTypeSystemNode *Parent) {
  return getChildrenAtOffset0(Parent).size();
}

// The strongly connected components of the type system over all its links.
//
// A node N reaches a pointer to one of N's ancestors exactly when the two are
// in the same component, since the pointer leads back to N through the
// ancestor. Redirecting a pointer removes paths and adds none: its new pointee
// was already reachable from the old one. So the components can only split,
// and only the one the redirected pointer is in: it is computed again, from
// its own nodes, the next time one of them is asked about.
class Components {
private:
  using Node = LayoutTypeSystemNode;

  llvm::DenseMap<const Node *, unsigned> Of;
  // The nodes of each component that has more than one
  llvm::DenseMap<unsigned, std::vector<Node *>> Members;
  llvm::DenseSet<unsigned> Changed;
  unsigned NextID = 0;

public:
  explicit Components(LayoutTypeSystem &TS) {
    std::vector<Node *> All;
    for (Node *N : llvm::nodes(&TS))
      All.push_back(N);
    split(All, std::nullopt);
  }

  // The component of N as it is now
  unsigned of(const Node *N) {
    unsigned ID = Of.lookup(N);
    if (not Changed.erase(ID))
      return ID;

    auto It = Members.find(ID);
    std::vector<Node *> Old = std::move(It->second);
    Members.erase(It);
    split(Old, ID);
    return Of.lookup(N);
  }

  // Whether Other is in the component with identifier ID, which of() just
  // returned for another node
  bool contains(unsigned ID, const Node *Other) const {
    return Of.lookup(Other) == ID;
  }

  // A link between two nodes of N's component went away
  void markChanged(const Node *N) { Changed.insert(Of.lookup(N)); }

private:
  // Tarjan's algorithm on Nodes, which are all the nodes of the type system,
  // or all those of component Within, whose links to other components cannot
  // be part of a cycle
  void split(const std::vector<Node *> &Nodes, std::optional<unsigned> Within) {
    struct Frame {
      Node *N;
      Node::NeighborIterator Next;
    };
    struct Numbers {
      unsigned Index;
      unsigned Low;
      bool OnStack;
    };
    llvm::DenseMap<const Node *, Numbers> Visited;
    std::vector<Frame> Visit;
    std::vector<Node *> Stack;

    auto Enter = [&](Node *N) {
      unsigned Index = Visited.size();
      Visited[N] = {Index, Index, true};
      Stack.push_back(N);
      Visit.push_back({N, N->successors().begin()});
    };

    for (Node *Root : Nodes) {
      if (Visited.count(Root))
        continue;

      Enter(Root);
      while (not Visit.empty()) {
        Node *N = Visit.back().N;
        if (Visit.back().Next != N->successors().end()) {
          Node *Successor = Visit.back().Next->first;
          ++Visit.back().Next;
          auto It = Visited.find(Successor);
          if (It == Visited.end()) {
            if (not Within.has_value() or Of.lookup(Successor) == *Within)
              Enter(Successor);
          } else if (It->second.OnStack) {
            Numbers &Mine = Visited[N];
            Mine.Low = std::min(Mine.Low, Visited[Successor].Index);
          }
          continue;
        }

        Visit.pop_back();
        Numbers Mine = Visited[N];
        if (not Visit.empty()) {
          Numbers &Above = Visited[Visit.back().N];
          Above.Low = std::min(Above.Low, Mine.Low);
        }

        if (Mine.Low != Mine.Index)
          continue;

        unsigned ID = NextID++;
        std::vector<Node *> Component;
        Node *Member = nullptr;
        do {
          Member = Stack.back();
          Stack.pop_back();
          Visited[Member].OnStack = false;
          Of[Member] = ID;
          Component.push_back(Member);
        } while (Member != N);

        if (Component.size() > 1)
          Members[ID] = std::move(Component);
      }
    }
  }
};

bool PushDownPointers::runOnTypeSystem(LayoutTypeSystem &TS) {

  bool Changed = false;

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  Components SCCs(TS);

  for (LayoutTypeSystemNode *Parent : llvm::nodes(&TS)) {

    if (isInstanceLeaf(Parent))
      continue;

    revng_assert(not isPointerNode(Parent));

    DFVisitSet GrandParentsAtOffset0 = getGrandParentsAtOffset0(Parent);
    llvm::SmallVector<LayoutTypeSystemNode *, 8> PointersToGrandParents;

    // The pointers to redirect point to one of the grandparents other than
    // Parent, so they are among the pointer predecessors of those, and Parent
    // reaches them: the ones in Parent's component.
    std::optional<unsigned> ParentComponent;
    for (const LayoutTypeSystemNode *GrandParent : GrandParentsAtOffset0) {
      if (GrandParent == Parent)
        continue;
      for (const auto &Link : GrandParent->predecessors()) {
        if (not isPointerEdge(Link))
          continue;
        if (not ParentComponent.has_value())
          ParentComponent = SCCs.of(Parent);
        if (SCCs.contains(*ParentComponent, Link.first))
          PointersToGrandParents.push_back(Link.first);
      }
    }

    if (not PointersToGrandParents.empty())
      SCCs.markChanged(Parent);

    for (LayoutTypeSystemNode *Pointer : PointersToGrandParents) {
      revng_assert(isPointerNode(Pointer) and
                   Pointer->successors().size() == 1);
      auto PointerEdgeIt = Pointer->successors().begin();
      LayoutTypeSystemNode *Pointee = PointerEdgeIt->first;
      LayoutTypeSystemNode *NewPointee = nullptr;
      DFVisitSet Visited = getChildrenAtOffset0(Parent);
      for (LayoutTypeSystemNode *N :
           llvm::depth_first_ext(Instance0Graph(Pointee), Visited)) {
        if (getNumChildrenAtOffset0(N) > 1) {
          NewPointee = N;
          break;
        }
      }

      if (not NewPointee)
        NewPointee = Parent;

      TS.eraseEdge(Pointer, PointerEdgeIt);
      TS.addPointerLink(Pointer, NewPointee);
      Changed = true;
    }
  }

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  return Changed;
}

} // end namespace dla
