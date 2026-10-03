//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DLASteps
bool init_unit_test();

#include <compare>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "boost/test/unit_test.hpp"

#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Progress.h"

#include "revng/DataLayoutAnalysis/DLATypeSystem.h"

#include "lib/DataLayoutAnalysis/Middleend/DLAStep.h"
#include "lib/DataLayoutAnalysis/Middleend/RemoveBackedges.h"

using LTSN = dla::LayoutTypeSystemNode;

using namespace llvm;
using namespace dla;

template<typename StepT>
static void runStep(LayoutTypeSystem &TS) {
  // Enable expensive checks
  VerifyLog.enable();

  // Run Step
  dla::StepManager SM;
  revng_check(SM.addStep<StepT>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();
}

static LTSN *createRoot(LayoutTypeSystem &TS, unsigned Size = 0U) {
  LTSN *Root = TS.createArtificialLayoutType();
  Root->Size = Size;

  return Root;
}

static LTSN *addInstanceAtOffset(LayoutTypeSystem &TS,
                                 LTSN *Parent,
                                 unsigned ChildOffset,
                                 unsigned ChildSize) {
  LTSN *Child = TS.createArtificialLayoutType();
  Child->Size = ChildSize;
  OffsetExpression OE{};
  OE.Offset = ChildOffset;
  TS.addInstanceLink(Parent, Child, std::move(OE));

  return Child;
}

// Adds a strided instance edge from Parent to Child, i.e. an array of TripCount
// elements spaced Stride bytes apart, starting at ChildOffset inside Parent.
static void addStridedInstance(LayoutTypeSystem &TS,
                               LTSN *Parent,
                               LTSN *Child,
                               unsigned ChildOffset,
                               unsigned Stride,
                               unsigned TripCount) {
  OffsetExpression OE{};
  OE.Offset = ChildOffset;
  OE.Strides.push_back(Stride);
  OE.TripCounts.push_back(TripCount);
  TS.addInstanceLink(Parent, Child, std::move(OE));
}

// Counts the instance edges going from Src to Tgt.
static unsigned countInstanceEdges(const LTSN *Src, const LTSN *Tgt) {
  unsigned Result = 0;
  for (const auto &Edge : Src->Successors)
    if (Edge.first == Tgt and isInstanceEdge(Edge))
      ++Result;
  return Result;
}

static LTSN *
addEquality(LayoutTypeSystem &TS, LTSN *Parent, unsigned ChildSize = 0U) {
  LTSN *Child = TS.createArtificialLayoutType();
  Child->Size = ChildSize;
  TS.addEqualityLink(Parent, Child);

  return Child;
}

static void checkNode(const LayoutTypeSystem &TS,
                      const LayoutTypeSystemNode *N,
                      const unsigned ExpectedSize,
                      const InterferingChildrenInfo ExpectedInfo,
                      const std::set<unsigned> &ExpectedEqClass) {
  const dla::VectEqClasses &Eq = TS.getEqClasses();
  revng_check(N);
  revng_check(not Eq.isRemoved(N->ID));
  revng_check(ExpectedSize == N->Size);
  revng_check(ExpectedInfo == N->InterferingInfo);

  const auto &EqClass = Eq.computeEqClass(N->ID);
  revng_check(EqClass.size() == ExpectedEqClass.size());
  for (auto &Collapsed : EqClass)
    revng_check(ExpectedEqClass.contains(Collapsed));
}

// Test cases

/// Test an equality CC with a pointer edge
BOOST_AUTO_TEST_CASE(CollapseEquality) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  LTSN *Node1 = addEquality(TS, Root);
  LTSN *Node2 = addEquality(TS, Root);
  LTSN *Node3 = addEquality(TS, Node2);
  TS.addEqualityLink(Node1, Node3);
  LTSN *PtrNode = createRoot(TS);
  TS.addPointerLink(Node3, PtrNode);

  runStep<dla::CollapseEqualitySCC>(TS);

  // Check that equality nodes have been collapsed
  revng_check(TS.getNumLayouts() == 2);
  revng_check(llvm::is_contained(TS.getLayoutsRange(), Node2));
  revng_check(llvm::is_contained(TS.getLayoutsRange(), PtrNode));
  revng_check(Node2->Successors.size() == 1);
  // Check that the pointer edge is still alive
  const auto &[Child, Tag] = *(llvm::children_edges<LTSN *>(Node2).begin());
  revng_check(Child == PtrNode);
  revng_check(Tag->getKind() == dla::TypeLinkTag::LK_Pointer);

  // Check Eq Classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  revng_check(Eq.getNumElements() == 5);
  revng_check(Eq.getNumClasses() == 2);
  checkNode(TS, Node2, 0, InterferingChildrenInfo::Unknown, { 0, 1, 2, 3 });
  checkNode(TS, PtrNode, 0, InterferingChildrenInfo::Unknown, { 4 });
}

/// Test an instance-at-offset-0 loop with a pointer edge
BOOST_AUTO_TEST_CASE(CollapseInstanceAtOffset0SCC_instance0) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  LTSN *Node1 = addInstanceAtOffset(TS, Root, 0, 0);
  LTSN *Node2 = addInstanceAtOffset(TS, Root, 0, 0);
  LTSN *Node3 = addInstanceAtOffset(TS, Node2, 0, 0);
  TS.addInstanceLink(Node3, Root, OffsetExpression{});
  LTSN *Node4 = addInstanceAtOffset(TS, Node1, 0, 0);

  LTSN *PtrNode = createRoot(TS);
  TS.addPointerLink(Node4, PtrNode);

  runStep<dla::CollapseInstanceAtOffset0SCC>(TS);
  // Check that instance-at-offset-0 loops have been collapsed
  revng_check(TS.getNumLayouts() == 4);
  revng_check(llvm::is_contained(TS.getLayoutsRange(), Node3));
  revng_check(llvm::is_contained(TS.getLayoutsRange(), Node1));
  revng_check(llvm::is_contained(TS.getLayoutsRange(), Node4));
  revng_check(llvm::is_contained(TS.getLayoutsRange(), PtrNode));
  revng_check(Node3->Successors.size() == 1);
  // Check that the pointer edge is still alive
  const auto &[Child, Tag] = *(llvm::children_edges<LTSN *>(Node4).begin());
  revng_check(Child == PtrNode);
  revng_check(Tag->getKind() == dla::TypeLinkTag::LK_Pointer);

  // Check Eq Classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  revng_check(Eq.getNumElements() == 6);
  revng_check(Eq.getNumClasses() == 4);
  checkNode(TS, Node3, 0, InterferingChildrenInfo::Unknown, { 0, 2, 3 });
  checkNode(TS, Node1, 0, InterferingChildrenInfo::Unknown, { 1 });
  checkNode(TS, Node4, 0, InterferingChildrenInfo::Unknown, { 4 });
  checkNode(TS, PtrNode, 0, InterferingChildrenInfo::Unknown, { 5 });
}

/// Test an instance-at-offset-0 CC with a backward
/// instance-at-offset-nonzero
BOOST_AUTO_TEST_CASE(CollapseInstanceSCC_instancenon0) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  LTSN *Node1 = addInstanceAtOffset(TS, Root, 0, 0);
  LTSN *Node2 = addInstanceAtOffset(TS, Root, 0, 0);
  LTSN *Node3 = addInstanceAtOffset(TS, Node2, 0, 0);
  TS.addInstanceLink(Node3, Root, OffsetExpression{ 8 });
  LTSN *Node4 = addInstanceAtOffset(TS, Node1, 0, 16);

  LTSN *PtrNode = createRoot(TS);
  TS.addPointerLink(Node4, PtrNode);

  // Run step
  runStep<dla::CollapseInstanceAtOffset0SCC>(TS);

  // Check that no node has been collapsed
  revng_check(TS.getNumLayouts() == 6);

  // Check that the pointer edge is still alive
  const auto &[Child, Tag] = *(llvm::children_edges<LTSN *>(Node4).begin());
  revng_check(Child == PtrNode);
  revng_check(Tag->getKind() == dla::TypeLinkTag::LK_Pointer);

  // Check that the instance back-edge has been removed
  revng_check(Node3->Successors.size() == 0);

  // Check Eq Classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  revng_check(Eq.getNumElements() == 6);
  revng_check(Eq.getNumClasses() == 6);
}

// ----------------- PruneLayoutNodesWithoutLayout ------

/// Test that branches without sized leaves are pruned
BOOST_AUTO_TEST_CASE(PruneLayoutNodesWithoutLayout_basic) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  std::vector<LTSN *> Level0 = {
    addInstanceAtOffset(TS, Root, /*offset=*/0, /*size=*/0),
    addInstanceAtOffset(TS, Root, /*offset=*/0, /*size=*/0)
  };
  std::vector<LTSN *> Level1 = {
    addInstanceAtOffset(TS, Level0[0], /*offset=*/0, /*size=*/0),
    addInstanceAtOffset(TS, Level0[0], /*offset=*/0, /*size=*/0),
    addInstanceAtOffset(TS, Level0[1], /*offset=*/0, /*size=*/0),
    addInstanceAtOffset(TS, Level0[1], /*offset=*/0, /*size=*/0)
  };
  std::vector<LTSN *> Leaves = {
    addInstanceAtOffset(TS, Level1[0], /*offset=*/0, /*size=*/8),
    addInstanceAtOffset(TS, Level1[1], /*offset=*/0, /*size=*/8),
    addInstanceAtOffset(TS, Level1[2], /*offset=*/0, /*size=*/0),
    addInstanceAtOffset(TS, Level1[3], /*offset=*/0, /*size=*/0)
  };
  // Add pointers
  std::vector<LTSN *> PtrNodes;
  for (LTSN *Leaf : Leaves) {
    LTSN *PtrNode = createRoot(TS);
    // PtrNode->Size = 8U;
    TS.addPointerLink(Leaf, PtrNode);
    PtrNodes.push_back(PtrNode);
  }

  // Run step
  runStep<dla::PruneLayoutNodesWithoutLayout>(TS);

  // Check final tree
  revng_check(TS.getNumLayouts() == 6);
  revng_check(Root->Successors.size() == 1);
  revng_check(llvm::is_contained(llvm::children<LTSN *>(Root), Level0[0]));
  revng_check(not llvm::is_contained(llvm::children<LTSN *>(Root), Level0[1]));

  revng_check(Level0[0]->Successors.size() == 2);
  revng_check(llvm::is_contained(llvm::children<LTSN *>(Level0[0]), Level1[0]));
  revng_check(llvm::is_contained(llvm::children<LTSN *>(Level0[0]), Level1[1]));

  revng_check(Level1[0]->Successors.size() == 1);
  revng_check(llvm::is_contained(llvm::children<LTSN *>(Level1[0]), Leaves[0]));
  revng_check(Level1[1]->Successors.size() == 1);
  revng_check(llvm::is_contained(llvm::children<LTSN *>(Level1[1]), Leaves[1]));

  revng_check(isLeaf(Leaves[0]) and isLeaf(Leaves[1]));

  // Check Eq Classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  revng_check(Eq.getNumElements() == 15);
  revng_check(Eq.getNumClasses() == 7);

  // All leaves should have been removed
  std::vector<unsigned> RemovedIDs = { 2, 5, 6, 9, 10, 11, 12, 13, 14 };
  for (auto ID : RemovedIDs)
    revng_check(Eq.isRemoved(ID));
}

// ----------------- CollapseSingleChild ----------------

/// Test nominal case with one parent and one instance child at offset 0
BOOST_AUTO_TEST_CASE(CollapseSingleChild_offsetZero) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 8U);
  /*LTSN *Child =*/addInstanceAtOffset(TS,
                                       Parent,
                                       /*offset=*/0U,
                                       /*size=*/8U);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check graph
  revng_check(TS.getNumLayouts() == 1);
  checkNode(TS, Parent, 8, InterferingChildrenInfo::Unknown, { 0, 1 });
}

/// Test we don't collapse if the size of the parent is different from the
/// child.
BOOST_AUTO_TEST_CASE(CollapseSingleChild_offsetZero_expected_dontcollapse) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 10U);
  LTSN *Child = addInstanceAtOffset(TS,
                                    Parent,
                                    /*offset=*/0U,
                                    /*size=*/8U);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check graph
  revng_check(TS.getNumLayouts() == 2);
  checkNode(TS, Parent, 10, InterferingChildrenInfo::Unknown, { 0 });
  checkNode(TS, Child, 8, InterferingChildrenInfo::Unknown, { 1 });
}

/// Test the case where there is only one child but with offset > 0
BOOST_AUTO_TEST_CASE(CollapseSingleChild_offsetNonZero) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 16U);
  LTSN *Child = addInstanceAtOffset(TS,
                                    Parent,
                                    /*offset=*/8U,
                                    /*size=*/8U);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check graph
  revng_check(TS.getNumLayouts() == 2);
  checkNode(TS, Parent, 16, InterferingChildrenInfo::Unknown, { 0 });
  checkNode(TS, Child, 8, InterferingChildrenInfo::Unknown, { 1 });
}

/// Test the case in which a node has many children (should not collapse)
BOOST_AUTO_TEST_CASE(CollapseSingleChild_multiChild) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 16U);
  LTSN *Child1 = addInstanceAtOffset(TS,
                                     Parent,
                                     /*offset=*/0U,
                                     /*size=*/8U);
  addInstanceAtOffset(TS,
                      Child1,
                      /*offset=*/0U,
                      /*size=*/8U);
  LTSN *Child2 = addInstanceAtOffset(TS,
                                     Parent,
                                     /*offset=*/8U,
                                     /*size=*/8U);
  addInstanceAtOffset(TS,
                      Child2,
                      /*offset=*/0U,
                      /*size=*/8U);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check graph
  revng_check(TS.getNumLayouts() == 3);
  checkNode(TS, Parent, 16, InterferingChildrenInfo::Unknown, { 0 });
  checkNode(TS, Child1, 8, InterferingChildrenInfo::Unknown, { 1, 2 });
  checkNode(TS, Child2, 8, InterferingChildrenInfo::Unknown, { 3, 4 });
}

/// Test the case in which there are multiple levels of single-childs to
/// be collapsed
BOOST_AUTO_TEST_CASE(CollapseSingleChild_multiLevel) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Level0 = createRoot(TS, 8U);
  LTSN *Level1 = addInstanceAtOffset(TS,
                                     Level0,
                                     /*offset=*/0U,
                                     /*size=*/8U);
  LTSN *Level2 = addInstanceAtOffset(TS,
                                     Level1,
                                     /*offset=*/0U,
                                     /*size=*/8U);
  LTSN *Level3 = addInstanceAtOffset(TS,
                                     Level2,
                                     /*offset=*/0U,
                                     /*size=*/8U);
  /*LTSN *Level4 = */ addInstanceAtOffset(TS,
                                          Level3,
                                          /*offset=*/0U,
                                          /*size=*/8U);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check graph
  revng_check(TS.getNumLayouts() == 1);
  checkNode(TS, Level0, 8, InterferingChildrenInfo::Unknown, { 0, 1, 2, 3, 4 });
}

// ----------------- ComputeUpperMemberAccesses ---------

/// Test member access computation
BOOST_AUTO_TEST_CASE(ComputeUpperMemberAccesses_basic) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  Root->Size = 0U;

  LTSN *Child1 = createRoot(TS);
  Child1->Size = 8U;
  TS.addInstanceLink(Root, Child1, OffsetExpression{});

  LTSN *Child2 = createRoot(TS);
  Child2->Size = 8U;
  OffsetExpression MultiOE;
  MultiOE.Offset = 10U;
  MultiOE.Strides = { 10U };
  MultiOE.TripCounts = { 3U };
  TS.addInstanceLink(Root, Child2, std::move(MultiOE));

  LTSN *Child3 = createRoot(TS);
  Child3->Size = 16U;
  OffsetExpression SingleOE;
  SingleOE.Offset = 30U;
  TS.addInstanceLink(Root, Child3, std::move(SingleOE));

  LTSN *Root2 = createRoot(TS);
  LTSN *PtrNode = createRoot(TS);
  PtrNode->Size = 8U;
  TS.addInstanceLink(Root2, PtrNode, OffsetExpression{});

  // Add pointer edge
  TS.addPointerLink(PtrNode, Root);

  // Run step
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  SM.run(TS);
  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check remaining nodes
  revng_check(TS.getNumLayouts() == 6);
  // Check edges
  unsigned NInstance = 0U;
  for (auto &[Child, Tag] : llvm::children_edges<LTSN *>(Root)) {
    switch (Tag->getKind()) {
    case TypeLinkTag::LK_Instance:
      NInstance++;
      break;
    default:
      revng_check(false);
    }
  }
  revng_check(NInstance == 3);
  // Check size
  revng_check(Root->Size == 46);
  revng_check(Root2->Size == 8);

  // Check Eq Classes
  revng_check(Eq.getNumElements() == 6);
  revng_check(Eq.getNumClasses() == 6);
  revng_check(not Eq.isRemoved(PtrNode->ID));
}

// ----------------- ComputeNonInterferingComponents ----

/// Test union nested inside a struct
BOOST_AUTO_TEST_CASE(ComputeNonInterferingComponents_basic) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Root = createRoot(TS);
  Root->Size = 0U;

  LTSN *Child1 = createRoot(TS);
  Child1->Size = 8U;
  TS.addInstanceLink(Root, Child1, OffsetExpression{});

  LTSN *Child2 = createRoot(TS);
  Child2->Size = 8U;
  OffsetExpression MultiOE;
  MultiOE.Offset = 10U;
  MultiOE.Strides = { 10U };
  MultiOE.TripCounts = { 3U };
  TS.addInstanceLink(Root, Child2, std::move(MultiOE));

  LTSN *Child3 = createRoot(TS);
  Child3->Size = 16U;
  OffsetExpression SingleOE;
  SingleOE.Offset = 30U;
  TS.addInstanceLink(Root, Child3, std::move(SingleOE));

  LTSN *Root2 = createRoot(TS);
  LTSN *PtrNode = createRoot(TS);
  PtrNode->Size = 8U;
  TS.addInstanceLink(Root2, PtrNode, OffsetExpression{});
  TS.addPointerLink(PtrNode, Root);
  LTSN *UnionNode = addInstanceAtOffset(TS, Root2, 0U, 8U);

  // Run step
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());
  SM.run(TS);
  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check remaining nodes
  revng_check(TS.getNumLayouts() == 8);
  // Check edges
  unsigned NInstance = 0U;
  for (auto &[Child, Tag] : llvm::children_edges<LTSN *>(Root)) {
    switch (Tag->getKind()) {
    case TypeLinkTag::LK_Instance:
      NInstance++;
      break;
    default:
      revng_check(false);
    }
  }
  revng_check(NInstance == 2);
  // Check size
  revng_check(Root->Size == 46);
  revng_check(Root2->Size == 8);
  revng_check(Root->InterferingInfo = AllChildrenAreNonInterfering);
  revng_check(Child1->InterferingInfo = AllChildrenAreNonInterfering);
  revng_check(Child2->InterferingInfo = AllChildrenAreNonInterfering);
  revng_check(Root2->InterferingInfo = AllChildrenAreInterfering);
  revng_check(PtrNode->InterferingInfo = AllChildrenAreNonInterfering);
  revng_check(UnionNode->InterferingInfo = AllChildrenAreNonInterfering);
  // Check new node
  const auto &[FinalChild1, Tag1] = *(Child2->Predecessors.begin());
  const auto &[FinalChild2, Tag2] = *(Child3->Predecessors.begin());
  revng_check(Tag1->getOffsetExpr().Offset == 0U);
  revng_check(Tag2->getOffsetExpr().Offset == 20U);

  revng_check(FinalChild1 == FinalChild2);

  // Check Eq Classes
  revng_check(Eq.getNumElements() == 8);
  revng_check(Eq.getNumClasses() == 8);
  revng_check(not Eq.isRemoved(PtrNode->ID));
}

// ----------------- Deduplicate Union Fields --------------

BOOST_AUTO_TEST_CASE(DeduplicateFields_basic) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *NodeA = createRoot(TS);
  LTSN *NodeB = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  /*LTSN *NodeD =*/addInstanceAtOffset(TS, NodeB, /*offset=*/0, /*size=*/8);
  /*LTSN *NodeE =*/addInstanceAtOffset(TS, NodeB, /*offset=*/8, /*size=*/8);

  LTSN *NodeC = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/16);
  LTSN *NodeF = addInstanceAtOffset(TS, NodeC, /*offset=*/0, /*size=*/8);
  LTSN *NodeG = addInstanceAtOffset(TS, NodeC, /*offset=*/8, /*size=*/8);

  LTSN *Node1 = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/12);
  LTSN *Node2 = addInstanceAtOffset(TS, Node1, /*offset=*/4, /*size=*/8);
  LTSN *Node3 = addInstanceAtOffset(TS, Node2, /*offset=*/0, /*size=*/4);
  LTSN *Node4 = addInstanceAtOffset(TS, Node2, /*offset=*/4, /*size=*/4);

  // Run steps
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<DeduplicateFields>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());

  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check TS
  revng_check(TS.getNumLayouts() == 7);
  checkNode(TS, NodeA, 16, AllChildrenAreInterfering, { 0 });
  checkNode(TS, NodeC, 16, AllChildrenAreNonInterfering, { 1, 4 });
  checkNode(TS, NodeF, 8, AllChildrenAreNonInterfering, { 2, 5 });
  checkNode(TS, NodeG, 8, AllChildrenAreNonInterfering, { 3, 6 });
  checkNode(TS, Node1, 8, AllChildrenAreNonInterfering, { 7, 8 });
  checkNode(TS, Node3, 4, AllChildrenAreNonInterfering, { 9 });
  checkNode(TS, Node4, 4, AllChildrenAreNonInterfering, { 10 });
}

BOOST_AUTO_TEST_CASE(DeduplicateFields_diamond) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *NodeA = createRoot(TS);
  LTSN *NodeB = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  NodeB->Size = 8;
  /*LTSN *NodeC =*/addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/8);
  LTSN *NodeD = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  LTSN *NodeE = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  LTSN *NodeF = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  LTSN *NodeG = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);

  LTSN *NodeH = addInstanceAtOffset(TS, NodeD, /*offset=*/0, /*size=*/8);
  OffsetExpression OE{};
  OE.Offset = 0;
  TS.addInstanceLink(NodeE, NodeH, std::move(OE));

  LTSN *NodeI = addInstanceAtOffset(TS, NodeF, /*offset=*/0, /*size=*/8);
  OffsetExpression OE2{};
  OE2.Offset = 0;
  TS.addInstanceLink(NodeG, NodeI, std::move(OE2));

  // Run steps
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<DeduplicateFields>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());

  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check TS
  revng_check(TS.getNumLayouts() == 1);
  checkNode(TS,
            NodeA,
            8,
            AllChildrenAreNonInterfering,
            { 0, 1, 2, 3, 4, 5, 6, 7, 8 });
}

BOOST_AUTO_TEST_CASE(DeduplicateFields_commonNodeSymmetric) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *NodeUnion = createRoot(TS);
  LTSN *NodeA = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  LTSN *NodeB = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/8);
  LTSN *NodeC = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  /*LTSN *NodeD =*/addInstanceAtOffset(TS, NodeC, /*offset=*/8, /*size=*/8);

  LTSN *NodeA1 = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  LTSN *NodeC1 = addInstanceAtOffset(TS, NodeA1, /*offset=*/0, /*size=*/0);
  /*LTSN *NodeD1 =*/addInstanceAtOffset(TS, NodeC1, /*offset=*/8, /*size=*/8);
  OffsetExpression OE{};
  OE.Offset = 0;
  TS.addInstanceLink(NodeA1, NodeB, std::move(OE));

  // Run steps
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<DeduplicateFields>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());

  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check TS
  revng_check(TS.getNumLayouts() == 3);
  checkNode(TS, NodeUnion, 16, AllChildrenAreNonInterfering, { 0, 1, 5 });
  checkNode(TS, NodeB, 8, AllChildrenAreNonInterfering, { 2 });
  checkNode(TS, NodeC1, 8, AllChildrenAreNonInterfering, { 3, 4, 6, 7 });
}

BOOST_AUTO_TEST_CASE(DeduplicateFields_commonNodeAsymmetric) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *NodeUnion = createRoot(TS);
  LTSN *NodeA = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  /*LTSN *NodeB =*/addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/8);
  LTSN *NodeC = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/0);
  LTSN *NodeD = addInstanceAtOffset(TS, NodeC, /*offset=*/4, /*size=*/8);

  LTSN *NodeA1 = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  LTSN *NodeC1 = addInstanceAtOffset(TS, NodeA1, /*offset=*/0, /*size=*/0);
  /*LTSN *NodeD1 =*/addInstanceAtOffset(TS, NodeC1, /*offset=*/4, /*size=*/8);
  OffsetExpression OE{};
  OE.Offset = 0;
  TS.addInstanceLink(NodeA1, NodeD, std::move(OE));

  // Run steps
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<DeduplicateFields>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());

  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check TS
  revng_check(TS.getNumLayouts() == 2);
  checkNode(TS, NodeUnion, 12, AllChildrenAreInterfering, { 0, 1, 5 });
  checkNode(TS, NodeC1, 8, AllChildrenAreNonInterfering, { 2, 3, 4, 6, 7 });
}

BOOST_AUTO_TEST_CASE(DeduplicateFields_commonNodeAsymmetricCollapse) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *NodeUnion = createRoot(TS);
  LTSN *NodeA = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  LTSN *NodeB = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/8);
  LTSN *NodeC = addInstanceAtOffset(TS, NodeA, /*offset=*/0, /*size=*/10);
  LTSN *NodeD = addInstanceAtOffset(TS, NodeC, /*offset=*/0, /*size=*/8);

  LTSN *NodeA1 = addInstanceAtOffset(TS, NodeUnion, /*offset=*/0, /*size=*/0);
  LTSN *NodeC1 = addInstanceAtOffset(TS, NodeA1, /*offset=*/0, /*size=*/0);
  /* LTSN *NodeD1 = */ addInstanceAtOffset(TS,
                                           NodeC1,
                                           /*offset=*/0,
                                           /*size=*/8);
  OffsetExpression OE{};
  OE.Offset = 0;
  TS.addInstanceLink(NodeA1, NodeD, std::move(OE));

  // Run steps
  VerifyLog.enable();
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<CollapseSingleChild>());
  revng_check(SM.addStep<DeduplicateFields>());
  revng_check(SM.addStep<ComputeNonInterferingComponents>());

  SM.run(TS);

  // Compress the equivalence classes
  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();

  // Check TS
  revng_check(TS.getNumLayouts() == 5);
  checkNode(TS, NodeUnion, 10, AllChildrenAreInterfering, { 0 });
  checkNode(TS, NodeA, 10, AllChildrenAreInterfering, { 1 });
  checkNode(TS, NodeB, 8, AllChildrenAreNonInterfering, { 2 });
  checkNode(TS, NodeC, 10, AllChildrenAreNonInterfering, { 3 });
  checkNode(TS, NodeA1, 8, AllChildrenAreNonInterfering, { 4, 5, 6, 7 });
}

// ----------------- ArrangeAccessesHierarchically ----------------

// Runs the steps ArrangeAccessesHierarchically depends on, then the step
// itself. VerifyLog is enabled, so the verifyDAG() assertions guarding the
// beginning and the end of the step are active.
static void runArrangeAccessesHierarchically(LayoutTypeSystem &TS) {
  VerifyLog.enable();

  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<DecomposeStridedEdges>());
  revng_check(SM.addStep<ArrangeAccessesHierarchically>());
  SM.run(TS);

  dla::VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();
}

/// Two edges describing the same bytes but ending on different nodes.
///
/// Parent reaches ChildA through a plain edge and ChildB through a strided one,
/// both at offset 0. getFieldSize spans the whole array for the strided edge
/// (3 * 16 + 8 = 56 bytes) rather than the 8-byte node, so the plain edge looks
/// contained in the strided one. Projected into one element of the strided edge
/// though, both are (offset 0, size 8), and ChildB is itself 8 bytes: the two
/// edges describe the same bytes, so neither is nested in the other.
///
/// Pushing one through the other regardless makes ChildB a parent of ChildA.
/// ChildA already reaches ChildB, so that closes a loop, and a loop here is
/// what later makes the model type construction recurse forever.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_sameBytesDifferentNodes) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);
  LTSN *ChildA = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
  LTSN *ChildB = createRoot(TS, 8U);
  addStridedInstance(TS,
                     Parent,
                     ChildB,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  // ChildA already reaches ChildB, so making ChildB a parent of ChildA would
  // close a loop.
  TS.addInstanceLink(ChildA, ChildB, OffsetExpression{});

  // A child reached only by Parent through a single plain edge is volatile, and
  // would be merged into Parent before any of this is looked at. A second
  // parent pins ChildA in place. ChildB is already pinned by its strided edge.
  LTSN *OtherParent = createRoot(TS, 8U);
  TS.addInstanceLink(OtherParent, ChildA, OffsetExpression{});

  // Neither child may be a leaf, otherwise the two edges are not comparable.
  addInstanceAtOffset(TS, ChildA, /*offset=*/0U, /*size=*/8U);
  addInstanceAtOffset(TS, ChildB, /*offset=*/0U, /*size=*/8U);

  runArrangeAccessesHierarchically(TS);

  // Both edges are still hanging off Parent, and ChildB has not become a
  // parent of ChildA.
  revng_check(countInstanceEdges(Parent, ChildA) == 1);
  revng_check(countInstanceEdges(Parent, ChildB) == 1);
  revng_check(countInstanceEdges(ChildB, ChildA) == 0);
  revng_check(countInstanceEdges(ChildA, ChildB) == 1);
}

/// The same shape, but both edges end on the same node.
///
/// Here the plain edge really is redundant with the first element of the
/// strided one, so the step drops it. This is intentionally left alone by the
/// check that handles the case above, and adding a link would be a no-op
/// anyway, since a node cannot be linked to itself.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_sameBytesSameNode) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);
  LTSN *Child = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
  addStridedInstance(TS,
                     Parent,
                     Child,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  // Child must not be a leaf, otherwise the two edges are not comparable.
  LTSN *Leaf = addInstanceAtOffset(TS, Child, /*offset=*/0U, /*size=*/8U);

  runArrangeAccessesHierarchically(TS);

  // The plain edge has been dropped and the strided one survives.
  revng_check(countInstanceEdges(Parent, Child) == 1);
  for (const auto &[Successor, Tag] : Parent->Successors)
    if (Successor == Child)
      revng_check(Tag->getOffsetExpr().Strides.size() == 1);

  // Child has not been made a child of itself.
  revng_check(countInstanceEdges(Child, Child) == 0);

  revng_check(Leaf != nullptr);
}

/// Two edges describing the same bytes, ending on different nodes, but with no
/// path from the inner one to the outer one.
///
/// This is the same shape as
/// ArrangeAccessesHierarchically_sameBytesDifferentNodes, except that ChildA
/// does not reach ChildB. Pushing the plain edge through the strided one makes
/// ChildB a parent of ChildA, which here closes nothing, so the push is
/// expected to go through. Declining it whenever two edges describe the same
/// bytes would needlessly give up a valid rearrangement.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_sameBytesNoLoop) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);
  LTSN *ChildA = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
  LTSN *ChildB = createRoot(TS, 8U);
  addStridedInstance(TS,
                     Parent,
                     ChildB,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  // Deliberately no edge from ChildA to ChildB here.

  // A child reached only by Parent through a single plain edge is volatile, and
  // would be merged into Parent before any of this is looked at. A second
  // parent pins ChildA in place. ChildB is already pinned by its strided edge.
  LTSN *OtherParent = createRoot(TS, 8U);
  TS.addInstanceLink(OtherParent, ChildA, OffsetExpression{});

  // Neither child may be a leaf, otherwise the two edges are not comparable.
  addInstanceAtOffset(TS, ChildA, /*offset=*/0U, /*size=*/8U);
  addInstanceAtOffset(TS, ChildB, /*offset=*/0U, /*size=*/8U);

  runArrangeAccessesHierarchically(TS);

  // The push went through: ChildA now hangs off ChildB instead of Parent.
  revng_check(countInstanceEdges(ChildB, ChildA) == 1);
  revng_check(countInstanceEdges(Parent, ChildA) == 0);
  revng_check(countInstanceEdges(Parent, ChildB) == 1);
}

/// Two pushes in the same batch, each of which is fine on its own, but which
/// together close a loop.
///
/// Parent reaches both ChildA and ChildB through a plain and a strided edge at
/// the same offset. That makes two pushes comparable:
///
///   the plain edge to ChildA fits in the strided edge to ChildB -> adds B -> A
///   the plain edge to ChildB fits in the strided edge to ChildA -> adds A -> B
///
/// Neither edge exists while the pushes are being decided, so a loop check
/// performed at that point sees nothing to complain about for either of them.
/// The edges are only applied afterwards, and together they close A <-> B.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_twoPushesCloseALoop) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);

  LTSN *ChildA = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
  addStridedInstance(TS,
                     Parent,
                     ChildA,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  LTSN *ChildB = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
  addStridedInstance(TS,
                     Parent,
                     ChildB,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  // Neither child may be a leaf, otherwise the edges are not comparable.
  addInstanceAtOffset(TS, ChildA, /*offset=*/0U, /*size=*/8U);
  addInstanceAtOffset(TS, ChildB, /*offset=*/0U, /*size=*/8U);

  runArrangeAccessesHierarchically(TS);

  // Whichever way the two edges are arranged, they must not point at each
  // other.
  bool AReachesB = countInstanceEdges(ChildA, ChildB) != 0;
  bool BReachesA = countInstanceEdges(ChildB, ChildA) != 0;
  revng_check(not(AReachesB and BReachesA));

  // A push that is skipped to avoid the loop must not drop the edge: whatever
  // wasn't pushed anywhere has to stay where it was.
  revng_check(countInstanceEdges(Parent, ChildA) != 0 or BReachesA);
  revng_check(countInstanceEdges(Parent, ChildB) != 0 or AReachesB);
}

/// The same shape as twoPushesCloseALoop, with three children instead of two.
///
/// With three of them a loop can have more than two edges in it: once B -> A
/// and C -> B have been added, adding A -> C closes A -> C -> B -> A. Detecting
/// that requires walking back more than a single hop from the node the edge is
/// being added to.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_threePushesCloseALoop) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);

  LTSN *Children[3];
  for (LTSN *&Child : Children) {
    Child = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);
    addStridedInstance(TS,
                       Parent,
                       Child,
                       /*offset=*/0U,
                       /*stride=*/16U,
                       /*tripCount=*/4U);

    // No child may be a leaf, otherwise the edges are not comparable.
    addInstanceAtOffset(TS, Child, /*offset=*/0U, /*size=*/8U);
  }

  // The instance graph being acyclic is checked by the step itself, since
  // runArrangeAccessesHierarchically enables VerifyLog. What's left to check
  // here is that skipping the pushes that would have closed a loop didn't drop
  // anything on the floor.
  runArrangeAccessesHierarchically(TS);

  for (const LTSN *Child : Children) {
    bool StillReachable = countInstanceEdges(Parent, Child) != 0;
    for (const LTSN *Other : Children)
      if (Other != Child)
        StillReachable |= countInstanceEdges(Other, Child) != 0;
    revng_check(StillReachable);
  }
}

/// An edge that spans the same bytes as two other edges, only one of which it
/// would close a loop with.
///
/// The edge to ChildA covers a whole element of the strided edge to ChildB and
/// of the strided edge to ChildC alike, so both are candidates to push it
/// through. ChildA already reaches ChildB, so that one has to be refused, while
/// the one through ChildC has to go ahead: refusing a push is per comparison,
/// not per edge.
BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchically_onlyLoopingPushIsRefused) {
  dla::LayoutTypeSystem TS;

  // Build TS
  LTSN *Parent = createRoot(TS, 56U);

  LTSN *ChildA = addInstanceAtOffset(TS, Parent, /*offset=*/0U, /*size=*/8U);

  LTSN *ChildB = createRoot(TS, 8U);
  addStridedInstance(TS,
                     Parent,
                     ChildB,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  LTSN *ChildC = createRoot(TS, 8U);
  addStridedInstance(TS,
                     Parent,
                     ChildC,
                     /*offset=*/0U,
                     /*stride=*/16U,
                     /*tripCount=*/4U);

  // This is what makes pushing the edge to ChildA through ChildB a loop.
  TS.addInstanceLink(ChildA, ChildB, OffsetExpression{});

  // A child reached only by Parent through a single plain edge is volatile, and
  // would be merged into Parent before any of this is looked at. A second
  // parent pins ChildA in place. ChildB and ChildC are already pinned by their
  // strided edges.
  LTSN *OtherParent = createRoot(TS, 8U);
  TS.addInstanceLink(OtherParent, ChildA, OffsetExpression{});

  // ChildC may not be a leaf, otherwise the two edges are not comparable.
  addInstanceAtOffset(TS, ChildC, /*offset=*/0U, /*size=*/8U);

  runArrangeAccessesHierarchically(TS);

  // The push that would have closed the loop was refused, and left the edge
  // that was already there alone.
  revng_check(countInstanceEdges(ChildB, ChildA) == 0);
  revng_check(countInstanceEdges(ChildA, ChildB) == 1);

  // The other one went through.
  revng_check(countInstanceEdges(ChildC, ChildA) == 1);
  revng_check(countInstanceEdges(Parent, ChildA) == 0);
}

//
// Collapsing SCCs in one traversal gives what one traversal per start gave
//

using GraphNodeT = LTSN *;
using EqualityNodeT = EdgeFilteredGraph<GraphNodeT, isEqualityEdge>;
using InstanceOffset0EdgeT = EdgeFilteredGraph<GraphNodeT, isInstanceOff0>;

static bool sizeGreater(const LTSN *LHS, const LTSN *RHS) {
  return LHS->Size > RHS->Size;
}

/// The collapse before it found the SCCs in one traversal: a fresh
/// llvm::scc_iterator from every node not yet visited
template<typename NodeT>
static bool referenceCollapseSCCs(LayoutTypeSystem &TS) {

  std::set<GraphNodeT> VisitedNodes;

  using scc_t = std::vector<GraphNodeT>;
  llvm::SmallVector<scc_t, 0> ToCollapse;

  // We cannot start just from the roots because we cannot exclude that there
  // are loops without entries.
  for (const auto &Node : llvm::nodes(&TS)) {
    revng_assert(Node != nullptr);
    if (VisitedNodes.contains(Node))
      continue;

    llvm::scc_iterator<NodeT> I = llvm::scc_begin(NodeT(Node));
    llvm::scc_iterator<NodeT> E = llvm::scc_end(NodeT(Node));
    for (const auto &SCC : llvm::make_range(I, E)) {
      revng_assert(not SCC.empty());
      if (VisitedNodes.contains(SCC[0]))
        continue;
      VisitedNodes.insert(SCC.begin(), SCC.end());

      if (SCC.size() > 1)
        ToCollapse.push_back(SCC);
    }
  }

  for (scc_t &SCC : ToCollapse) {
    revng_assert(not SCC.empty());
    // Reorder the SCC so that nodes with the same size are adjacent.
    // Nodes with larger sizes come first.
    llvm::stable_sort(SCC, sizeGreater);

    // Now, merge all the nodes that have the same size, while noting the
    // leaders.
    llvm::SmallSetVector<LayoutTypeSystemNode *, 8> Leaders;
    {
      auto LeaderIt = SCC.begin();
      auto CurrentSize = (*LeaderIt)->Size;
      auto End = SCC.end();

      while (LeaderIt != End) {

        Leaders.insert(*LeaderIt);

        // Find the first node with a size different from CurrentSize.
        auto EndCurrentSize = std::next(LeaderIt);
        while (EndCurrentSize != End and (*EndCurrentSize)->Size == CurrentSize)
          ++EndCurrentSize;

        // Merge all the nodes with the same size.
        TS.mergeNodes({ &*LeaderIt, &*EndCurrentSize });

        // Go to the next size.
        LeaderIt = EndCurrentSize;
      }
    }

    // Now, if the last Leader has Size 0, we merge it with the first Leader,
    // which represents the largest type.
    if (Leaders.size() > 1 and Leaders.back()->Size == 0) {
      TS.mergeNodes(llvm::SmallVector{ Leaders.front(), Leaders.back() });
      Leaders.pop_back();
    }

    // If a single leader is left, we're done, all the nodes have been merged
    // together.
    if (Leaders.size() < 2)
      continue;

    // If we're left with more than one leader, we just want to remove all edges
    // between them and replace them with offset-at-0 edges, from the larger
    // leader to the smaller.
    auto LeaderIt = Leaders.begin();
    auto LeaderEnd = Leaders.end();
    for (; LeaderIt != LeaderEnd; ++LeaderIt) {

      // For each leader we erase all the non-pointer edges going to other
      // leaders.
      auto *Leader = *LeaderIt;
      using DLAGraph = llvm::GraphTraits<LayoutTypeSystemNode *>;
      auto EdgeIt = DLAGraph::child_edge_begin(Leader);
      auto EdgeEnd = DLAGraph::child_edge_end(Leader);
      auto EdgeNext = DLAGraph::child_edge_end(Leader);

      for (; EdgeIt != EdgeEnd; EdgeIt = EdgeNext) {

        EdgeNext = std::next(EdgeIt);
        auto &Edge = *EdgeIt;

        // Skip pointers
        if (isPointerEdge(Edge))
          continue;

        const auto &[Child, Tag] = Edge;

        // Skip edges that go to non-leaders
        if (not Leaders.contains(Child))
          continue;

        TS.eraseEdge(Leader, EdgeIt);
      }

      // Then, for each leader, we add an instance-at-offset-0 edge to the next
      // leader, which is the one with immediately lower size.
      if (auto NextLeaderIt = std::next(LeaderIt); NextLeaderIt != LeaderEnd)
        TS.addInstanceLink(*LeaderIt, *NextLeaderIt, dla::OffsetExpression{});
    }
  }

  return ToCollapse.size();
}

/// The backedge removal before it skipped nodes known to reach no loop,
/// verbatim
namespace reference {

static Logger Log("dla-remove-backedges-reference");

using LTSN = LayoutTypeSystemNode;

template<typename T>
concept SCCWithBackedgeHelper = requires {
  typename T::SCCNodeView;
  typename T::BackedgeNodeView;
};

struct InstanceOffsetZeroWithInstanceBackedge {
  using SCCNodeView = EdgeFilteredGraph<const LTSN *, isInstanceOff0>;
  using BackedgeNodeView = EdgeFilteredGraph<const LTSN *, isInstanceOffNon0>;
};

template<SCCWithBackedgeHelper SCC>
static bool isMixedEdge(const llvm::GraphTraits<LTSN *>::EdgeRef &E) {
  return SCC::SCCNodeView::filter()(E) or SCC::BackedgeNodeView::filter()(E);
}

template<SCCWithBackedgeHelper SCC>
static bool isSCCLeaf(const LTSN *N) {
  using Graph = llvm::GraphTraits<typename SCC::SCCNodeView>;
  return Graph::child_begin(N) == Graph::child_end(N);
}

template<SCCWithBackedgeHelper SCC>
static bool isSCCRoot(const LTSN *N) {
  using Inverse = llvm::Inverse<typename SCC::SCCNodeView>;
  using InverseGraph = llvm::GraphTraits<Inverse>;
  return InverseGraph::child_begin(N) == InverseGraph::child_end(N);
}

template<SCCWithBackedgeHelper SCC>
static bool hasNoSCCEdge(const LTSN *Node) {
  return isSCCRoot<SCC>(Node) and isSCCLeaf<SCC>(Node);
};

template<SCCWithBackedgeHelper SCC>
using MixedNodeT = EdgeFilteredGraph<LTSN *, isMixedEdge<SCC>>;

template<SCCWithBackedgeHelper SCC>
struct StackEntry {
  LTSN *Node = nullptr;
  const LTSN *ComponentLeader = nullptr;
  typename MixedNodeT<SCC>::ChildEdgeIteratorType NextToVisitIt;
};

struct EdgeInfo {
  LTSN *Src = nullptr;
  LTSN *Tgt = nullptr;
  const TypeLinkTag *Tag = nullptr;
  // Comparison operators to use in set
  std::strong_ordering operator<=>(const EdgeInfo &) const = default;
};

template<SCCWithBackedgeHelper SCC>
static bool removeBackedgesFromSCC(LayoutTypeSystem &TS) {
  bool Changed = false;
  if (VerifyLog.isEnabled()) {
    revng_assert(TS.verifyConsistency());

    // Verify that the graph is a DAG looking only at SCCNodeView
    std::set<const LTSN *> Visited;
    for (const auto &Node : llvm::nodes(&TS)) {
      revng_assert(Node != nullptr);
      if (Visited.contains(Node))
        continue;

      auto I = llvm::scc_begin(typename SCC::SCCNodeView(Node));
      auto E = llvm::scc_end(typename SCC::SCCNodeView(Node));
      for (; I != E; ++I) {
        Visited.insert(I->begin(), I->end());
        if (I.hasCycle())
          revng_check(false);
      }
    }
  }

  revng_log(Log, "Removing Backedges From Loops");

  llvm::Task T(2, "removeBackedgesFromSCC");
  T.advance("Detect SCC Node View Components");
  // Assign each node to a Component, except for those that have no incoming nor
  // outgoing SCCNodeView edges. The goal is to identify the subsets of nodes
  // that are connected by means of SCCNodeView edges. In this way we divide the
  // graph in subgraphs, such that for each pair of nodes P and Q with (P != Q)
  // in the same sugraphs (i.e. with the same component) P is reachable from Q
  // looking only at **undirected** SCCNodeView edges. Each of this subgraphs is
  // called "component". The idea is that SCCNodeView edges are more meaningful
  // than SCCBackedgeView edges, so we don't want to remove any of them, but we
  // need to identify such edges that create loops across multiple components,
  // and cut them.
  llvm::EquivalenceClasses<const LTSN *> Components;
  {
    revng_log(Log, "Detect components");
    for (const LTSN *N : llvm::nodes(&TS)) {
      revng_assert(N != nullptr);
      revng_log(Log, "N->ID: " << N->ID);
      LoggerIndent Indent{ Log };

      for (const LTSN *Child : llvm::children<typename SCC::SCCNodeView>(N)) {
        revng_log(Log, "Merging with SCCNodeView Child with ID: " << Child->ID);
        Components.unionSets(N, Child);
      }
    }
  }

  if (Log.isEnabled()) {
    revng_log(Log, "Detected components:");
    LoggerIndent Indent{ Log };
    for (auto I = Components.begin(), E = Components.end(); I != E;
         ++I) { // Iterate over all of the equivalence sets.

      if (!I->isLeader()) {
        // Ignore non-leader sets.
        continue;
      }

      revng_log(Log,
                "Component for Node with ID: "
                  << (*Components.findLeader(I))->ID);
      LoggerIndent MoreIndent{ Log };
      // Loop over members in this set.
      for (const LTSN *N : llvm::make_range(Components.member_begin(I),
                                            Components.member_end()))
        revng_log(Log, "ID: " << N->ID);
    }
  }

  // Here all the nodes are nodes have a component, except nodes that have no
  // incoming or outgoing SCCNodeView edges

  T.advance("Remove Backedges");
  using MixedNodeT = MixedNodeT<SCC>;

  // Visit the nodes twice. The first pass starts from SCCNodeView roots.
  // Merging nodes can also create loops that no SCCNodeView root reaches, for
  // example a loop whose only entries are pointer edges. The second pass starts
  // from every node no search has reached yet, sharing one visited set, so it
  // breaks those loops too while visiting each node once.
  auto Nodes = llvm::to_vector(llvm::nodes(&TS));
  llvm::SmallPtrSet<const LTSN *, 16> Explored;
  for (size_t Index = 0; Index < 2 * Nodes.size(); ++Index) {
    LTSN *Root = Nodes[Index % Nodes.size()];
    revng_assert(Root != nullptr);
    bool SecondPass = Index >= Nodes.size();
    if (Index == 0 or Index == Nodes.size())
      revng_log(Log, (SecondPass ? "## Second pass" : "## First pass"));

    if (SecondPass) {
      if (Explored.contains(Root))
        continue;
    } else {
      // We start from SCCNodeView roots and look if we find an SCC with mixed
      // edges.
      if (hasNoSCCEdge<SCC>(Root))
        continue;

      if (not isSCCRoot<SCC>(Root))
        continue;
    }

    revng_log(Log, "# Looking for mixed loops from: " << Root->ID);

    using StackEntry = StackEntry<SCC>;

    llvm::SmallPtrSet<const LTSN *, 16> RootVisited;
    auto &Visited = SecondPass ? Explored : RootVisited;
    llvm::SmallPtrSet<const LTSN *, 16> InStack;
    llvm::SmallVector<EdgeInfo> BackedgesOnStack;
    llvm::SmallVector<StackEntry> VisitStack;

    // Helper enum to describe if push succeeds and different ways to fail
    enum class TryPushStatus {
      FailedNoChildren,
      FailedCloseLoop,
      AlreadyVisited,
      Success,
    };

    // Struct holding the success/failure status, along with the edge iterator
    // that caused it.
    struct TryPushResult {
      TryPushStatus Status;
      MixedNodeT::ChildEdgeIteratorType EdgeToPush;
    };

    const auto TryPushNextChild = [&]() -> TryPushResult {
      revng_assert(not VisitStack.empty());
      revng_assert(VisitStack.size() == InStack.size());
      StackEntry &Top = VisitStack.back();
      const auto &[TopNode, TopComponent, _] = Top;
      // Make a copy, because we have to increment the old Top.NextToVisitIt,
      // while at the same time we still want to have access to the previous one
      // in case of failure.
      typename MixedNodeT::ChildEdgeIteratorType
        NextEdgeToVisit = Top.NextToVisitIt;

      revng_log(Log, "--* TryPushNextChild of (" << TopNode->ID << ')');
      LoggerIndent Indent{ Log };

      if (NextEdgeToVisit == MixedNodeT::child_edge_end(TopNode)) {
        revng_log(Log, "--| no children left");
        return TryPushResult{ .Status = TryPushStatus::FailedNoChildren,
                              .EdgeToPush = NextEdgeToVisit };
      }

      ++Top.NextToVisitIt;

      LTSN *NextChild = NextEdgeToVisit->first;
      const TypeLinkTag *NextTag = NextEdgeToVisit->second;
      revng_log(Log,
                "--| next children is " << NextChild->ID
                                        << " tag: " << *NextTag);

      bool NewVisit = Visited.insert(NextChild).second;
      if (not NewVisit) {
        if (InStack.contains(NextChild)) {
          revng_log(Log, "--| loop detected!");
          return TryPushResult{ .Status = TryPushStatus::FailedCloseLoop,
                                .EdgeToPush = NextEdgeToVisit };
        } else {
          revng_log(Log, "--| already visited!");
          return TryPushResult{ .Status = TryPushStatus::AlreadyVisited,
                                .EdgeToPush = NextEdgeToVisit };
        }
      }

      // Make sure we track all the backedges on stack, because if we ever hit
      // a loop we'll have to mark these for removal.
      EdgeInfo NewEdge = { TopNode, NextChild, NextTag };
      if (SCC::BackedgeNodeView::filter()({ nullptr, NewEdge.Tag }))
        BackedgesOnStack.push_back(std::move(NewEdge));

      // Add a new entry on the stack
      revng_log(Log, "TopComponent: " << TopComponent);

      StackEntry NewEntry = {
        .Node = NextChild,
        .ComponentLeader = Components.findValue(NextChild) != Components.end() ?
                             *Components.findLeader(NextChild) :
                             TopComponent,
        .NextToVisitIt = MixedNodeT::child_edge_begin(NextChild)
      };

      VisitStack.push_back(std::move(NewEntry));
      InStack.insert(NextChild);
      revng_assert(InStack.size() == VisitStack.size());

      revng_log(Log, "--> pushed!");
      return TryPushResult{ .Status = TryPushStatus::Success,
                            .EdgeToPush = NextEdgeToVisit };
    };

    const auto Pop = [&]() {
      revng_assert(InStack.size() == VisitStack.size());

      // Make a copy before popping.
      StackEntry Back = VisitStack.back();
      revng_log(Log, "<-- pop(" << Back.Node->ID << ')');

      // Erase the node and the StackEntry from stack
      bool Erased = InStack.erase(Back.Node);
      revng_assert(Erased);
      VisitStack.pop_back();

      // If the stack is now empty, we're done.
      if (VisitStack.empty())
        return;

      // If after popping, the stack isn't empty, we have to check if we've
      // popped a backedge, and in that case remove it from BackedgesOnStack.
      StackEntry &Parent = VisitStack.back();
      auto Begin = MixedNodeT::child_edge_begin(Parent.Node);
      revng_assert(Parent.NextToVisitIt != Begin);

      EdgeInfo PoppedEdge = {
        .Src = Parent.Node,
        .Tgt = Back.Node,
        .Tag = std::prev(Parent.NextToVisitIt)->second,
      };

      // If we've popped a backedge it must be equal to PoppedEdge. If it's not
      // the stack is malformed. If it's the correct edge, we have to remove it
      // from BackedgesOnStack since we're popping.
      if (SCC::BackedgeNodeView::filter()({ nullptr, PoppedEdge.Tag })) {
        revng_assert(BackedgesOnStack.back() == PoppedEdge);
        BackedgesOnStack.pop_back();
      }
    };

    llvm::SmallSet<EdgeInfo, 8> ToRemove;

    bool HasComponent = Components.findValue(Root) != Components.end();
    revng_assert(SecondPass or HasComponent);
    StackEntry Init = { .Node = Root,
                        .ComponentLeader = HasComponent ?
                                             Components.getLeaderValue(Root) :
                                             Root,
                        .NextToVisitIt = MixedNodeT::child_edge_begin(Root) };
    VisitStack.push_back(std::move(Init));
    InStack.insert(Root);
    Visited.insert(Root);

    while (not VisitStack.empty()) {

      StackEntry &Top = VisitStack.back();

      revng_log(Log, "## Stack top has ID: " << Top.Node->ID);
      revng_log(Log, "   Component ID: " << Top.ComponentLeader->ID);

      const auto &[Status, Edge] = TryPushNextChild();
      switch (Status) {

      default:
        // do nothing
        break;

      case TryPushStatus::FailedNoChildren: {
        Pop();
      } break;

      case TryPushStatus::FailedCloseLoop: {

        // If we're closing a loop, add all the backedges involved in the loop
        // in the edges ToRemove.
        if (Log.isEnabled()) {
          for (EdgeInfo &B : BackedgesOnStack) {
            revng_log(Log,
                      "remove backedge that is on stack: "
                        << B.Src->ID << " -> " << B.Tgt->ID
                        << " Tag: " << B.Tag);
          }
        }
        ToRemove.insert(BackedgesOnStack.begin(), BackedgesOnStack.end());

        if (SCC::BackedgeNodeView::filter()({ nullptr, Edge->second })) {
          // This means that the edge E we tried to push on the stack is an
          // BackedgeNodeView edge closing a loop.
          EdgeInfo BackedgeNotPushed{
            .Src = VisitStack.back().Node,
            .Tgt = Edge->first,
            .Tag = Edge->second,
          };
          revng_log(Log,
                    "remove backedge closing loop, that isn't on stack: "
                      << BackedgeNotPushed.Src->ID << " -> "
                      << BackedgeNotPushed.Tgt->ID
                      << " Tag: " << BackedgeNotPushed.Tag);
          ToRemove.insert(std::move(BackedgeNotPushed));
        }

      } break;
      }
    }

    // Actually remove the edges
    for (auto &[Pred, Child, T] : ToRemove) {
      using Edge = LTSN::NeighborsSet::value_type;
      revng_log(Log,
                "# Removing backedge: " << Pred->ID << " -> " << Child->ID);
      revng_assert(SCC::BackedgeNodeView::filter()({ Pred, T }));
      Edge ChildToPred = std::make_pair(Pred, T);
      bool Erased = Child->Predecessors.erase(ChildToPred);
      revng_assert(Erased);
      Edge PredToChild = std::make_pair(Child, T);
      Erased = Pred->Successors.erase(PredToChild);
      revng_assert(Erased);
      Changed = true;
    }

    if (not SecondPass)
      Explored.insert(RootVisited.begin(), RootVisited.end());
  }

  if (VerifyLog.isEnabled()) {
    revng_assert(TS.verifyConsistency());

    // Verify that the graph is a DAG looking both at SCCNodeView and
    // BackedgeNodeView
    std::set<const LTSN *> Visited;
    for (const auto &Node : llvm::nodes(&TS)) {
      revng_assert(Node != nullptr);
      if (Visited.contains(Node))
        continue;

      auto I = llvm::scc_begin(MixedNodeT(Node));
      auto E = llvm::scc_end(MixedNodeT(Node));
      for (; I != E; ++I) {
        Visited.insert(I->begin(), I->end());
        if (I.hasCycle()) {
          for (const auto *N : *I)
            llvm::dbgs() << std::to_string(N->ID) << "\n";
          revng_check(false);
        }
      }
    }
  }

  return Changed;
}

static bool
removeInstanceBackedgesFromInstanceAtOffset0Loops(LayoutTypeSystem &TS) {
  return removeBackedgesFromSCC<InstanceOffsetZeroWithInstanceBackedge>(TS);
}

} // namespace reference

/// Everything the collapse can change: nodes, sizes, edges and equivalence
/// classes
static std::string describe(LayoutTypeSystem &TS) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  auto PrintEdges = [&OS](const LTSN::NeighborsSet &Edges) {
    for (const auto &[Other, Tag] : Edges) {
      OS << " " << Other->ID << ":" << Tag->getKind();
      if (Tag->getKind() == TypeLinkTag::LK_Instance)
        Tag->getOffsetExpr().print(OS);
    }
  };
  for (const LTSN *N : llvm::nodes(&TS)) {
    OS << N->ID << " size " << N->Size << " info " << N->InterferingInfo
       << " nonscalar " << N->NonScalar << " successors";
    PrintEdges(N->Successors);
    OS << " predecessors";
    PrintEdges(N->Predecessors);
    OS << "\n";
  }
  VectEqClasses &Eq = TS.getEqClasses();
  Eq.compress();
  for (unsigned I = 0; I < Eq.getNumElements(); ++I) {
    auto Class = Eq.getEqClassID(I);
    OS << I << " class " << (Class ? std::to_string(*Class) : "removed")
       << "\n";
  }
  return Result;
}

/// A random type system: nodes of a few sizes joined by equality,
/// instance-at-offset-0, other instance and pointer edges in the proportions
/// \p Weights, with about \p EdgesPerNode edges per node. One node in five is
/// a pointer, with at most one edge out, a pointer edge.
static void buildRandomTypeSystem(LayoutTypeSystem &TS,
                                  unsigned Seed,
                                  size_t NodeCount,
                                  double EdgesPerNode,
                                  std::array<unsigned, 4> Weights) {
  std::mt19937 Generator(Seed);
  const uint64_t Sizes[] = { 0, 1, 2, 4, 8, 8, 16 };
  std::vector<LTSN *> Nodes;
  for (size_t I = 0; I < NodeCount; ++I) {
    LTSN *N = TS.createArtificialLayoutType();
    N->Size = Sizes[std::uniform_int_distribution<size_t>(0, 6)(Generator)];
    Nodes.push_back(N);
  }

  std::uniform_int_distribution<size_t> PickNode(0, NodeCount - 1);
  std::discrete_distribution<unsigned> PickKind(Weights.begin(),
                                                Weights.end());
  auto IsPointer = [](const LTSN *N) { return N->ID % 5 == 4; };
  size_t EdgeCount = EdgesPerNode * NodeCount;
  for (size_t I = 0; I < EdgeCount; ++I) {
    LTSN *Src = Nodes[PickNode(Generator)];
    LTSN *Tgt = Nodes[PickNode(Generator)];
    unsigned Kind = PickKind(Generator);
    if (IsPointer(Src) != (Kind == 3) or (Kind == 0 and IsPointer(Tgt)))
      continue;
    if (Kind == 3 and not Src->Successors.empty())
      continue;
    switch (Kind) {
    case 0:
      TS.addEqualityLink(Src, Tgt);
      break;
    case 1:
      TS.addInstanceLink(Src, Tgt, OffsetExpression{});
      break;
    case 2:
      TS.addInstanceLink(Src,
                         Tgt,
                         OffsetExpression{ std::uniform_int_distribution<
                           uint64_t>(1, 16)(Generator) });
      break;
    default:
      TS.addPointerLink(Src, Tgt);
      break;
    }
  }
}

/// Chains in which every node reaches all the earlier ones, so a traversal per
/// start walks everything again from each node, closed into cycles every
/// \p CycleEvery nodes
static void buildBackwardChains(LayoutTypeSystem &TS,
                                bool Equality,
                                size_t NodeCount,
                                size_t CycleEvery) {
  std::vector<LTSN *> Nodes;
  for (size_t I = 0; I < NodeCount; ++I) {
    LTSN *N = TS.createArtificialLayoutType();
    N->Size = (I * 7) % 3 == 0 ? 8 : (I % 2 ? 4 : 0);
    Nodes.push_back(N);
  }
  auto Link = [&](LTSN *Src, LTSN *Tgt) {
    if (Equality)
      TS.addEqualityLink(Src, Tgt);
    else
      TS.addInstanceLink(Src, Tgt, OffsetExpression{});
  };
  for (size_t I = 1; I < NodeCount; ++I) {
    Link(Nodes[I], Nodes[I - 1]);
    if (I % CycleEvery == 0)
      Link(Nodes[I - CycleEvery / 2], Nodes[I]);
    if (I % 5 == 0)
      TS.addInstanceLink(Nodes[I], Nodes[I / 2], OffsetExpression{ 4 });
  }
}

template<typename BuilderT>
static void checkCollapseMatchesReference(BuilderT Build) {
  // Equality SCCs
  {
    LayoutTypeSystem Reference, Actual;
    Build(Reference);
    Build(Actual);
    bool ReferenceChanged = referenceCollapseSCCs<EqualityNodeT>(Reference);
    bool ActualChanged = CollapseEqualitySCC().runOnTypeSystem(Actual);
    revng_check(ReferenceChanged == ActualChanged);
    revng_check(describe(Reference) == describe(Actual));
  }

  // Instance-at-offset-0 SCCs, then the backedge removal that runs with them
  {
    LayoutTypeSystem Reference, Actual;
    Build(Reference);
    Build(Actual);
    bool ReferenceChanged = referenceCollapseSCCs<InstanceOffset0EdgeT>(
      Reference);
    ReferenceChanged |= reference::
      removeInstanceBackedgesFromInstanceAtOffset0Loops(Reference);
    bool ActualChanged = CollapseInstanceAtOffset0SCC().runOnTypeSystem(
      Actual);
    revng_check(ReferenceChanged == ActualChanged);
    revng_check(describe(Reference) == describe(Actual));
  }
}

BOOST_AUTO_TEST_CASE(CollapseSCCsMatchesOneTraversalPerStart) {
  for (bool Equality : { true, false })
    for (size_t CycleEvery : { 2, 3, 8, 1000 })
      checkCollapseMatchesReference([&](LayoutTypeSystem &TS) {
        buildBackwardChains(TS, Equality, 300, CycleEvery);
      });

  const std::array<std::array<unsigned, 4>, 4> Mixes = { {
    { 1, 1, 1, 1 },
    { 0, 4, 1, 1 },
    { 3, 1, 0, 1 },
    { 0, 6, 3, 0 },
  } };
  for (unsigned Seed = 0; Seed < 400; ++Seed) {
    size_t NodeCount = 1 + Seed % 97;
    double EdgesPerNode = 0.5 + (Seed % 7) * 0.5;
    checkCollapseMatchesReference([&](LayoutTypeSystem &TS) {
      buildRandomTypeSystem(TS,
                            Seed,
                            NodeCount,
                            EdgesPerNode,
                            Mixes[Seed % Mixes.size()]);
    });
  }
}

/// Roots that all reach one long chain of instance edges, with loops closed
/// by instance edges at an offset other than 0 every \p LoopEvery nodes, so
/// that the backedge removal searches the chain again from every root
static void buildRootsOverSharedChain(LayoutTypeSystem &TS,
                                      size_t RootCount,
                                      size_t ChainLength,
                                      size_t LoopEvery) {
  std::vector<LTSN *> Chain;
  for (size_t I = 0; I < ChainLength; ++I) {
    LTSN *N = TS.createArtificialLayoutType();
    N->Size = 8;
    Chain.push_back(N);
  }
  for (size_t I = 1; I < ChainLength; ++I) {
    if (I % 3 == 0)
      TS.addInstanceLink(Chain[I - 1], Chain[I], OffsetExpression{});
    else
      TS.addInstanceLink(Chain[I - 1], Chain[I], OffsetExpression{ I % 7 + 1 });
    if (I % LoopEvery == 0)
      TS.addInstanceLink(Chain[I], Chain[I - LoopEvery / 2],
                         OffsetExpression{ 2 });
  }
  for (size_t I = 0; I < RootCount; ++I) {
    LTSN *Root = TS.createArtificialLayoutType();
    Root->Size = 16;
    TS.addInstanceLink(Root, Chain[(I * 13) % ChainLength], OffsetExpression{});
    if (I % 4 == 0)
      TS.addInstanceLink(Root, Chain[(I * 7) % ChainLength],
                         OffsetExpression{ 8 });
  }
}

/// A random graph whose instance-at-offset-0 edges form a DAG, as they do
/// after their SCCs are collapsed, with other instance edges anywhere. One node
/// in five is a pointer, with at most one edge out, a pointer edge.
static void buildRandomOffset0DAG(LayoutTypeSystem &TS,
                                  unsigned Seed,
                                  size_t NodeCount,
                                  double EdgesPerNode) {
  std::mt19937 Generator(Seed);
  std::vector<LTSN *> Nodes;
  for (size_t I = 0; I < NodeCount; ++I) {
    LTSN *N = TS.createArtificialLayoutType();
    N->Size = 8;
    Nodes.push_back(N);
  }
  std::uniform_int_distribution<size_t> PickNode(0, NodeCount - 1);
  std::discrete_distribution<unsigned> PickKind({ 4, 3, 1 });
  auto IsPointer = [](size_t Index) { return Index % 5 == 4; };
  size_t EdgeCount = EdgesPerNode * NodeCount;
  for (size_t I = 0; I < EdgeCount; ++I) {
    size_t Src = PickNode(Generator);
    size_t Tgt = PickNode(Generator);
    unsigned Kind = PickKind(Generator);
    if (IsPointer(Src) != (Kind == 2))
      continue;
    if (Kind == 2 and not Nodes[Src]->Successors.empty())
      continue;
    switch (Kind) {
    case 0:
      if (Src < Tgt)
        TS.addInstanceLink(Nodes[Src], Nodes[Tgt], OffsetExpression{});
      break;
    case 1:
      TS.addInstanceLink(Nodes[Src],
                         Nodes[Tgt],
                         OffsetExpression{ std::uniform_int_distribution<
                           uint64_t>(1, 16)(Generator) });
      break;
    default:
      TS.addPointerLink(Nodes[Src], Nodes[Tgt]);
      break;
    }
  }
}

template<typename BuilderT>
static void checkBackedgeRemovalMatchesReference(BuilderT Build) {
  LayoutTypeSystem Reference, Actual;
  Build(Reference);
  Build(Actual);
  bool ReferenceChanged = reference::
    removeInstanceBackedgesFromInstanceAtOffset0Loops(Reference);
  bool ActualChanged = removeInstanceBackedgesFromInstanceAtOffset0Loops(
    Actual);
  revng_check(ReferenceChanged == ActualChanged);
  revng_check(describe(Reference) == describe(Actual));
}

BOOST_AUTO_TEST_CASE(RemoveBackedgesMatchesSearchFromEveryRoot) {
  for (size_t LoopEvery : { 2, 5, 16, 1000 })
    checkBackedgeRemovalMatchesReference([&](LayoutTypeSystem &TS) {
      buildRootsOverSharedChain(TS, 60, 200, LoopEvery);
    });

  for (unsigned Seed = 0; Seed < 1000; ++Seed) {
    size_t NodeCount = 2 + Seed % 89;
    double EdgesPerNode = 0.5 + (Seed % 9) * 0.4;
    checkBackedgeRemovalMatchesReference([&](LayoutTypeSystem &TS) {
      buildRandomOffset0DAG(TS, Seed, NodeCount, EdgesPerNode);
    });
  }
}
