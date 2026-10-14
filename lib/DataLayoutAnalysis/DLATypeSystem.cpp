//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/DataLayoutAnalysis/DLALayouts.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;

using NodeAllocatorT = SpecificBumpPtrAllocator<dla::LayoutTypeSystemNode>;

static Logger CollapsedNodePrinter("dla-print-collapsed-in-dot");

void *operator new(size_t, NodeAllocatorT &NodeAllocator);
void *operator new(size_t, NodeAllocatorT &NodeAllocator) {
  return NodeAllocator.Allocate();
}

namespace dla {

void OffsetExpression::print(llvm::raw_ostream &OS) const {
  OS << "Off: " << Offset;
  auto NStrides = Strides.size();
  revng_assert(NStrides == TripCounts.size());
  if (not Strides.empty()) {
    for (decltype(NStrides) N = 0; N < NStrides; ++N) {
      OS << ", {S:" << Strides[N] << ",TC:";
      if (TripCounts[N].has_value())
        OS << TripCounts[N].value();
      else
        OS << "none";
      OS << '}';
    }
  }
}

void LayoutTypePtr::print(raw_ostream &Out) const {
  Out << '{';
  Out << "0x";
  Out.write_hex(reinterpret_cast<const unsigned long long>(V));
  Out << " [";
  if (isa<Function>(V)) {
    Out << "fname: " << V->getName();
  } else {
    if (auto *I = dyn_cast<Instruction>(V))
      Out << "In Func: " << I->getFunction()->getName() << " Instr: ";
    else if (auto *A = dyn_cast<Argument>(V))
      Out << "In Func: " << A->getParent()->getName() << " Arg: ";

    Out.write_escaped(getName(V));
  }
  Out << "], 0x";
  Out.write_hex(FieldIdx);
  Out << '}';
}

std::string LayoutTypePtr::toString() const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  print(OS);
  return S;
}

void LayoutTypeSystemNode::print(llvm::raw_ostream &OS) const {
  OS << "LTSN ID: " << ID;
}

namespace {

static constexpr size_t str_len(const char *S) {
  return S ? (*S ? (1 + str_len(S + 1)) : 0UL) : 0UL;
}

// We use \l here instead of \n, because graphviz has this sick way of saying
// that the text in the node labels should be left-justified
static constexpr const char DoRet[] = "\\l";
static constexpr const char NoRet[] = "";
static_assert(sizeof(DoRet) == (str_len(DoRet) + 1));
static_assert(sizeof(NoRet) == (str_len(NoRet) + 1));

static constexpr const char Equal[] = "Equal";
static constexpr const char Instance[] = "Instance of: ";
static constexpr const char Pointer[] = "Points to: ";
static constexpr const char Unexpected[] = "Unexpected!";
static_assert(sizeof(Equal) == (str_len(Equal) + 1));
static_assert(sizeof(Instance) == (str_len(Instance) + 1));
static_assert(sizeof(Unexpected) == (str_len(Unexpected) + 1));
} // end unnamed namespace

void debug_function LayoutTypeSystem::dumpDotOnFile(const char *FName,
                                                    bool ShowCollapsed) const {
  std::error_code EC;
  raw_fd_ostream DotFile(FName, EC);
  revng_check(not EC, (EC.message() + ": " + FName).c_str());

  DotFile << "digraph LayoutTypeSystem {\n";
  DotFile << "  // List of nodes\n";

  for (const LayoutTypeSystemNode *L : getLayoutsRange()) {

    DotFile << "  node_" << L->ID << " [shape=rect,label=\"NODE ID: " << L->ID
            << " Size: " << L->Size << " InterferingChild: ";

    llvm::SmallVector<const llvm::Use *, 8> PtrUses;
    switch (L->InterferingInfo) {
    case Unknown:
      DotFile << 'U';
      break;
    case AllChildrenAreInterfering:
      DotFile << 'A';
      break;
    case AllChildrenAreNonInterfering:
      DotFile << 'N';
      break;
    default:
      revng_unreachable();
    }

    DotFile << " NonScalar: " << L->NonScalar;
    if (L->IsTypeRoot)
      DotFile << " TypeRoot: 1";
    if (L->HasUnknownPointee)
      DotFile << " UnknownPointee: 1";

    if (CollapsedNodePrinter.isEnabled() or ShowCollapsed)
      DebugPrinter->printNodeContent(*this, L, DotFile);

    DotFile << "\"];\n";
  }

  DotFile << "  // List of edges\n";

  for (LayoutTypeSystemNode *L : getLayoutsRange()) {

    uint64_t SrcNodeId = L->ID;

    for (const auto &PredP : L->predecessors()) {
      const TypeLinkTag *PredTag = PredP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and PredTag == OtherPair.second;
      };
      revng_assert(std::any_of(PredP.first->successors().begin(),
                               PredP.first->successors().end(), SameLink));
    }

    std::string Extra;
    std::string Color;
    std::string Style;
    for (const auto &SuccP : L->successors()) {
      const TypeLinkTag *EdgeTag = SuccP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and EdgeTag == OtherPair.second;
      };
      revng_assert(std::any_of(SuccP.first->predecessors().begin(),
                               SuccP.first->predecessors().end(), SameLink));
      const auto *TgtNode = SuccP.first;
      const char *EdgeLabel = nullptr;
      size_t LabelSize = 0;
      Extra.clear();
      Style.clear();
      switch (EdgeTag->getKind()) {
      case TypeLinkTag::LK_Equality: {
        EdgeLabel = Equal;
        LabelSize = sizeof(Equal) - 1;
        Color = ",color=green";
      } break;
      case TypeLinkTag::LK_Instance: {
        EdgeLabel = Instance;
        LabelSize = sizeof(Instance) - 1;
        Extra = dumpToString(EdgeTag->getOffsetExpr());
        Color = ",color=blue";
      } break;
      case TypeLinkTag::LK_Pointer: {
        EdgeLabel = Pointer;
        LabelSize = sizeof(Pointer) - 1;
        Color = ",color=purple";
        Style = ",style=dashed";
      } break;
      default: {
        EdgeLabel = Unexpected;
        LabelSize = sizeof(Unexpected) - 1;
        Color = ",color=red";
      } break;
      }
      DotFile << "  node_" << SrcNodeId << " -> node_" << TgtNode->ID
              << " [label=\"" << StringRef(EdgeLabel, LabelSize) << Extra
              << "\"" << Color << Style << "];\n";
    }
  }

  DotFile << "}\n";
}

LayoutTypeSystemNode *LayoutTypeSystem::createArtificialLayoutType() {
  using LTSN = LayoutTypeSystemNode;
  LTSN *New = new (NodeAllocator) LayoutTypeSystemNode(NID);
  revng_assert(New);
  ++NID;
  EqClasses.growBy1();
  bool Success = Layouts.insert(New).second;
  revng_assert(Success);
  return New;
}

SmallVector<LayoutTypeSystemNode *, 2>
LayoutTypeSystem::createArtificialLayoutTypes(unsigned N) {
  llvm::SmallVector<LayoutTypeSystemNode *, 2> Result;
  while (N--)
    Result.push_back(createArtificialLayoutType());
  return Result;
}

// After this many owner changes, the links to a slot are all repositioned, so
// that looking one up tries a bounded number of IDs.
static constexpr size_t MaxPastIDs = 32;

void LayoutTypeSystem::fixPredSucc(LayoutTypeSystemNode *From,
                                   LayoutTypeSystemNode *Into) {
  using LinkSlot = LayoutTypeSystemNode::LinkSlot;
  using LinkSet = LayoutTypeSystemNode::LinkSet;

  revng_assert(From != Into);

  // The node with more links keeps its slot and its link sets, whichever of
  // the two it is: the links to it stay as they are, and the links to the
  // other node, which are fewer, are rewritten.
  auto Degree = [](const LayoutTypeSystemNode *N) {
    return N->Successors.size() + N->Predecessors.size();
  };
  bool KeepFrom = Degree(From) > Degree(Into);
  LinkSlot *Keep = (KeepFrom ? From : Into)->Slot.get();
  LinkSlot *Drop = (KeepFrom ? Into : From)->Slot.get();

  // The IDs under which a link to Drop can be positioned
  llvm::SmallVector<uint64_t, 4> DropIDs{Drop->Owner->ID};
  DropIDs.append(Drop->PastIDs.begin(), Drop->PastIDs.end());

  if (KeepFrom) {
    std::swap(Into->Successors, From->Successors);
    std::swap(Into->Predecessors, From->Predecessors);
    std::swap(Into->Slot, From->Slot);
    Keep->PastIDs.push_back(From->ID);
    Keep->Owner = Into;

    // The change of owner puts out of position the links to Keep, which are
    // in its neighbors' sets. Keep's own sets do not hold any, apart from
    // links to itself, which go away below: if they were in order, they still
    // are, and a chain of merges into one node after another does not make
    // every read of them check them again.
    bool SuccessorsChecked =
        Into->Successors.CheckedAt == LinkSet::OwnerChanges;
    bool PredecessorsChecked =
        Into->Predecessors.CheckedAt == LinkSet::OwnerChanges;
    ++LinkSet::OwnerChanges;
    if (SuccessorsChecked)
      Into->Successors.CheckedAt = LinkSet::OwnerChanges;
    if (PredecessorsChecked)
      Into->Predecessors.CheckedAt = LinkSet::OwnerChanges;
  }

  // From now on Into has Keep and its link sets, and From has Drop's.
  auto FindDropEntry = [&](LinkSet &Links, const TypeLinkTag *Tag) {
    auto It = Links.Links.end();
    for (uint64_t ID : DropIDs) {
      It = Links.findAt(Drop, ID, Tag);
      if (It != Links.Links.end())
        break;
    }
    revng_assert(It != Links.Links.end());
    return It;
  };

  // Each neighbor of Drop gets a link to Keep in place of its link to Drop
  for (const auto &Entry : From->Successors.Links) {
    if (Entry.Slot == Keep or Entry.Slot == Drop)
      continue;
    LinkSet &Back = Entry.Slot->Owner->Predecessors;
    Back.Links.erase(FindDropEntry(Back, Entry.Tag));
    Back.insertEntry(Keep, Entry.Tag);
  }
  for (const auto &Entry : From->Predecessors.Links) {
    if (Entry.Slot == Keep or Entry.Slot == Drop)
      continue;
    LinkSet &Back = Entry.Slot->Owner->Successors;
    Back.Links.erase(FindDropEntry(Back, Entry.Tag));
    Back.insertEntry(Keep, Entry.Tag);
  }

  // Into gets Drop's links, except those that would lead to itself. When
  // they were Into's own, their entries move as they are, and replace an equal
  // link of From's: an iterator to a link of Into stays valid across a merge,
  // as it does when Into keeps its own sets.
  auto Absorb = [&](LinkSet &Target, LinkSet &Source) {
    while (not Source.Links.empty()) {
      auto Handle = Source.Links.extract(Source.Links.begin());
      LinkSet::Entry &Entry = Handle.value();
      if (Entry.Slot == Keep or Entry.Slot == Drop)
        continue;
      auto Existing = Target.findEntry(Entry.Slot, Entry.Tag);
      if (Existing != Target.Links.end()) {
        if (not KeepFrom)
          continue;
        Target.Links.erase(Existing);
      }
      Entry.Position = LinkSet::currentID(Entry.Slot);
      bool Inserted = Target.Links.insert(std::move(Handle)).inserted;
      revng_assert(Inserted);
    }
  };
  Absorb(Into->Successors, From->Successors);
  Absorb(Into->Predecessors, From->Predecessors);

  // Remove the links between the two nodes, and from each to itself
  for (LinkSet *Links : {&Into->Successors, &Into->Predecessors}) {
    Links->eraseEntriesOf(Keep);
    for (uint64_t ID : DropIDs)
      Links->eraseEntriesAt(Drop, ID);
  }

  if (Keep->PastIDs.size() >= MaxPastIDs) {
    for (const auto &Entry : Into->Successors.Links)
      Entry.Slot->Owner->Predecessors.reposition(Keep, Entry.Tag);
    for (const auto &Entry : Into->Predecessors.Links)
      Entry.Slot->Owner->Successors.reposition(Keep, Entry.Tag);
    Keep->PastIDs.clear();
  }
}

static Logger MergeLog("dla-merge-nodes");

void LayoutTypeSystem::mergeNodes(llvm::ArrayRef<LayoutTypeSystemNode *>
                                    ToMerge) {
  revng_assert(ToMerge.size() > 0ULL);
  // If we're merging just one node there's nothing to do.
  if (ToMerge.size() <= 1ULL)
    return;

  LayoutTypeSystemNode *Into = ToMerge[0];
  const unsigned IntoID = Into->ID;

  for (LayoutTypeSystemNode *From : llvm::drop_begin(ToMerge, 1)) {
    revng_assert(From != Into);
    revng_log(MergeLog, "Merging: " << From->ID << " Into: " << Into->ID);

    Into->InterferingInfo = Unknown;
    if (Into->NonScalar or From->NonScalar) {
      revng_assert(not(Into->NonScalar and From->NonScalar)
                   or Into->Size == From->Size);
      auto *NonScalar = Into->NonScalar ? Into : From;
      auto *Other = Into->NonScalar ? From : Into;
      revng_assert(Other->Size <= NonScalar->Size);
      Into->Size = NonScalar->Size;
    } else {
      revng_assert(not Into->Size or From->Size <= Into->Size);
      Into->Size = std::max(Into->Size, From->Size);
    }
    Into->NonScalar |= From->NonScalar;
    Into->IsTypeRoot |= From->IsTypeRoot;
    Into->HasUnknownPointee |= From->HasUnknownPointee;

    EqClasses.join(IntoID, From->ID);

    fixPredSucc(From, Into);

    // Remove From from Layouts
    bool Erased = Layouts.erase(From);
    revng_assert(Erased);
    From->~LayoutTypeSystemNode();
    NodeAllocator.Deallocate(From);
  }
}

void LayoutTypeSystem::removeNode(LayoutTypeSystemNode *ToRemove) {
  // Join the node's eq class with the removed class
  uint64_t TheID = ToRemove->ID;
  EqClasses.remove(TheID);
  revng_log(MergeLog, "Removing " << ToRemove->ID << "\n");

  LayoutTypeSystemNode::LinkSlot *Slot = ToRemove->Slot.get();
  for (const auto &Entry : ToRemove->Successors.Links)
    if (Entry.Slot != Slot)
      Entry.Slot->Owner->Predecessors.eraseEntriesOf(Slot);

  for (const auto &Entry : ToRemove->Predecessors.Links)
    if (Entry.Slot != Slot)
      Entry.Slot->Owner->Successors.eraseEntriesOf(Slot);

  bool Erased = Layouts.erase(ToRemove);
  revng_assert(Erased);
  ToRemove->~LayoutTypeSystemNode();
  NodeAllocator.Deallocate(ToRemove);
}

using NeighborIterator = LayoutTypeSystem::NeighborIterator;

void LayoutTypeSystem::moveEdgeTargetWithoutSumming(
    LayoutTypeSystemNode *OldTgt, LayoutTypeSystemNode *NewTgt,
    NeighborIterator InverseEdgeIt) {

  // First, move the successor from OldTgt to NewTgt
  LayoutTypeSystemNode *Src = InverseEdgeIt->first;
  const TypeLinkTag *Tag = InverseEdgeIt->second;
  bool Erased = Src->Successors.eraseEntry(OldTgt->Slot.get(), Tag);
  revng_assert(Erased);
  Src->Successors.insertEntry(NewTgt->Slot.get(), Tag);

  // Then, move the predecessor edge from OldTgt to NewTgt
  OldTgt->Predecessors.Links.erase(InverseEdgeIt.It);
  NewTgt->Predecessors.insertEntry(Src->Slot.get(), Tag);
}

void LayoutTypeSystem::moveEdgeSourceWithoutSumming(
    LayoutTypeSystemNode *OldSrc, LayoutTypeSystemNode *NewSrc,
    NeighborIterator EdgeIt) {
  // First, move the predecessor edge from OldSrc to NewSrc.
  LayoutTypeSystemNode *Tgt = EdgeIt->first;
  const TypeLinkTag *Tag = EdgeIt->second;
  bool Erased = Tgt->Predecessors.eraseEntry(OldSrc->Slot.get(), Tag);
  revng_assert(Erased);
  Tgt->Predecessors.insertEntry(NewSrc->Slot.get(), Tag);

  // Then, move the successor edge from OldSrc to NewSrc
  OldSrc->Successors.Links.erase(EdgeIt.It);
  NewSrc->Successors.insertEntry(Tgt->Slot.get(), Tag);
}

void LayoutTypeSystem::moveEdgeTarget(LayoutTypeSystemNode *OldTgt,
                                      LayoutTypeSystemNode *NewTgt,
                                      NeighborIterator InverseEdgeIt,
                                      int64_t OffsetToSum) {

  if (not OldTgt or not NewTgt)
    return;

  if (not OffsetToSum)
    return moveEdgeTargetWithoutSumming(OldTgt, NewTgt, InverseEdgeIt);

  LayoutTypeSystemNode *Src = InverseEdgeIt->first;

  // Erase info in Src that represent the fact that OldTgt was a successor.
  const TypeLinkTag *EdgeTag = InverseEdgeIt->second;
  bool Erased = Src->Successors.eraseEntry(OldTgt->Slot.get(), EdgeTag);
  revng_assert(Erased);

  // Remove the predecessor edge to be moved from OldTgt to NewTgt
  OldTgt->Predecessors.Links.erase(InverseEdgeIt.It);

  // Add new instance links with adjusted offsets from Src to NewTgt.
  switch (EdgeTag->getKind()) {

  case TypeLinkTag::LK_Instance: {
    OffsetExpression NewOE = EdgeTag->getOffsetExpr();
    NewOE.Offset += OffsetToSum;
    addInstanceLink(Src, NewTgt, std::move(NewOE));
  } break;

  case TypeLinkTag::LK_Equality:
  case TypeLinkTag::LK_Pointer:
  default:
    revng_unreachable("unexpected edge kind");
  }
}

void LayoutTypeSystem::moveEdgeSource(LayoutTypeSystemNode *OldSrc,
                                      LayoutTypeSystemNode *NewSrc,
                                      NeighborIterator EdgeIt,
                                      int64_t OffsetToSum) {

  if (not OldSrc or not NewSrc)
    return;

  if (not OffsetToSum)
    return moveEdgeSourceWithoutSumming(OldSrc, NewSrc, EdgeIt);

  LayoutTypeSystemNode *Tgt = EdgeIt->first;

  // Erase info in Tgt that represent the fact that OldSrc was a predecessor.
  const TypeLinkTag *EdgeTag = EdgeIt->second;
  bool Erased = Tgt->Predecessors.eraseEntry(OldSrc->Slot.get(), EdgeTag);
  revng_assert(Erased);

  // Remove the successor edge to be moved from OldSrc to NewSrc
  OldSrc->Successors.Links.erase(EdgeIt.It);

  // Add new instance links with adjusted offsets from NewSrc to Tgt.
  switch (EdgeTag->getKind()) {

  case TypeLinkTag::LK_Instance: {
    OffsetExpression NewOE = EdgeTag->getOffsetExpr();
    NewOE.Offset += OffsetToSum;
    addInstanceLink(NewSrc, Tgt, std::move(NewOE));
  } break;

  case TypeLinkTag::LK_Equality:
  case TypeLinkTag::LK_Pointer:
  default:
    revng_unreachable("unexpected edge kind");
  }
}

NeighborIterator LayoutTypeSystem::eraseEdge(LayoutTypeSystemNode *Src,
                                             NeighborIterator EdgeIt) {
  LayoutTypeSystemNode *Tgt = EdgeIt->first;

  // Erase the inverse edge from Tgt to Src
  bool Erased = Tgt->Predecessors.eraseEntry(Src->Slot.get(), EdgeIt->second);
  revng_assert(Erased);

  // Erase the actual forward edge from Src to Tgt
  return NeighborIterator(Src->Successors.Links.erase(EdgeIt.It));
}

void LayoutTypeSystem::dropOutgoingEdges(LayoutTypeSystemNode *N) {
  auto It = N->successors().begin();
  auto End = N->successors().end();
  while (It != End)
    It = eraseEdge(N, It);
}

void LayoutTypeSystem::checkLinks() const {
  using LinkSet = LayoutTypeSystemNode::LinkSet;
  using LinkSlot = LayoutTypeSystemNode::LinkSlot;

  for (LayoutTypeSystemNode *Node : Layouts) {
    revng_check(Node->Slot->Owner == Node);
    const std::pair<const LinkSet *, bool> Directions[] = {
        {&Node->Successors, true}, {&Node->Predecessors, false}};
    for (const auto &[Links, Forward] : Directions) {
      std::vector<std::pair<const LinkSlot *, const TypeLinkTag *>> Seen;
      for (const auto &Entry : Links->Links) {
        LayoutTypeSystemNode *Neighbor = Entry.Slot->Owner;
        revng_check(Neighbor != Node);
        revng_check(Layouts.contains(Neighbor));
        revng_check(Neighbor->Slot.get() == Entry.Slot);
        revng_check(Entry.Position == Neighbor->ID or
                    llvm::is_contained(Entry.Slot->PastIDs, Entry.Position));
        const LinkSet &Back =
            Forward ? Neighbor->Predecessors : Neighbor->Successors;
        revng_check(Back.findEntry(Node->Slot.get(), Entry.Tag) !=
                    Back.Links.end());
        Seen.push_back({Entry.Slot, Entry.Tag});
      }
      llvm::sort(Seen);
      revng_check(std::adjacent_find(Seen.begin(), Seen.end()) == Seen.end());

      Links->ensureOrdered();
      const LinkSet::Entry *Previous = nullptr;
      for (const auto &Entry : Links->Links) {
        revng_check(Entry.Position == Entry.Slot->Owner->ID);
        revng_check(Previous == nullptr or
                    LinkSet::EntryComparison()(*Previous, Entry));
        Previous = &Entry;
      }
    }
  }
}

static Logger VerifyDLALog("dla-verify-strict");

bool LayoutTypeSystem::verifyConsistency() const {
  for (LayoutTypeSystemNode *NodePtr : Layouts) {
    if (not NodePtr) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
    // Check that predecessors and successors are consistent
    for (auto &P : NodePtr->predecessors()) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      if (!P.first->successors().contains({NodePtr, P.second})) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
    for (auto &P : NodePtr->successors()) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      if (!P.first->predecessors().contains({NodePtr, P.second})) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Check that there are no self-edges
    for (auto &P : NodePtr->predecessors()) {
      LayoutTypeSystemNode *Pred = P.first;
      if (Pred == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    for (auto &P : NodePtr->successors()) {
      LayoutTypeSystemNode *Succ = P.first;
      if (Succ == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Verify that pointers are not also structs or unions
    unsigned NonPtrChildren = 0U;
    bool IsPointer = false;
    for (const auto &Edge : NodePtr->successors()) {
      if (isPointerEdge(Edge)) {
        // Node with outgoing pointer edges, only have one pointer edge.
        if (IsPointer) {
          if (VerifyDLALog.isEnabled())
            revng_check(false);
          return false;
        }
        IsPointer = true;
      } else if (isInstanceEdge(Edge)) {
        NonPtrChildren++;
      }

      if (IsPointer and NonPtrChildren > 0) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyDAG() const {
  if (not verifyConsistency())
    return false;

  if (not verifyInstanceDAG())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using NonPointerFilterT = EdgeFilteredGraph<LayoutTypeSystemNode *,
                                                isNotPointerEdge>;

    auto I = scc_begin(NonPointerFilterT(Node));
    auto E = scc_end(NonPointerFilterT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyInstanceDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceEdge>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyPointerDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using PointerNodeT = EdgeFilteredGraph<GraphNodeT, isPointerEdge>;
    auto I = scc_begin(PointerNodeT(Node));
    auto E = scc_end(PointerNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyNoEquality() const {
  if (not verifyConsistency())
    return false;
  for (const auto &Node : llvm::nodes(this)) {
    using LTSN = LayoutTypeSystemNode;
    for (const auto &Edge : llvm::children_edges<const LTSN *>(Node)) {
      if (isEqualityEdge(Edge)) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyInstanceAtOffset0DAG() const {
  if (not verifyConsistency())
    return false;

  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceOff0>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyLeafs() const {
  for (const auto &Node : llvm::nodes(this)) {
    if (isLeaf(Node) and Node->Size == 0) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyUnions() const {
  using GraphNodeT = const LayoutTypeSystemNode *;
  for (GraphNodeT Node : llvm::nodes(this)) {
    if (Node->InterferingInfo == AllChildrenAreInterfering and
        Node->successors().size() <= 1) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

unsigned VectEqClasses::growBy1() {
  ++NElems;
  grow(NElems);
  return NElems;
}

void VectEqClasses::remove(const unsigned A) {
  if (RemovedID)
    join(A, *RemovedID);
  else
    RemovedID = A;
}

bool VectEqClasses::isRemoved(const unsigned ID) const {
  // No removed nodes
  if (not RemovedID)
    return false;

  // Uncompressed map
  if (getNumClasses() == 0)
    return (findLeader(ID) == findLeader(*RemovedID));

  // Compressed map
  unsigned ElementEqClass = lookupEqClass(ID);
  unsigned RemovedEqClass = lookupEqClass(*RemovedID);
  return (ElementEqClass == RemovedEqClass);
}

std::optional<unsigned> VectEqClasses::getEqClassID(const unsigned ID) const {
  unsigned EqID = lookupEqClass(ID);
  bool IsRemoved = (RemovedID) ? lookupEqClass(*RemovedID) == EqID : false;

  if (IsRemoved)
    return {};
  return EqID;
}

std::vector<unsigned>
VectEqClasses::computeEqClass(const unsigned ElemID) const {
  std::vector<unsigned> EqClass;

  for (unsigned OtherID = 0; OtherID < NElems; OtherID++)
    if (haveSameEqClass(ElemID, OtherID))
      EqClass.push_back(OtherID);

  return EqClass;
}

bool VectEqClasses::haveSameEqClass(unsigned ID1, unsigned ID2) const {
  // Uncompressed map
  if (getNumClasses() == 0)
    return findLeader(ID1) == findLeader(ID2);

  // Compressed map
  return lookupEqClass(ID1) == lookupEqClass(ID2);
}

void TSDebugPrinter::printNodeContent(const LayoutTypeSystem &TS,
                                      const LayoutTypeSystemNode *N,
                                      llvm::raw_fd_ostream &File) const {
  auto EqClasses = TS.getEqClasses();

  File << DoRet;
  if (EqClasses.isRemoved(N->ID))
    File << "Removed" << DoRet;

  File << "Equivalence Class: [";
  for (auto ID : EqClasses.computeEqClass(N->ID))
    File << ID << ", ";
  File << "]" << DoRet;
}

void writeToLog(Logger &Log, const TypeLinkTag &Tag, int /* Ignore */) {

  switch (Tag.getKind()) {
  case TypeLinkTag::LK_Equality: {
    Log << Equal;
  } break;
  case TypeLinkTag::LK_Instance: {
    Log << Instance << dumpToString(Tag.getOffsetExpr());
  } break;
  case TypeLinkTag::LK_Pointer: {
    Log << Pointer;
  } break;
  default: {
    Log << Unexpected;
  } break;
  }
}

} // end namespace dla
