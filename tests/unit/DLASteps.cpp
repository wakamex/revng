//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DLASteps
bool init_unit_test();

#include "llvm/IR/Verifier.h"
#include <compare>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "boost/test/unit_test.hpp"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Progress.h"

#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/LocalVariables/LocalVariableHelpers.h"
#include "revng/Model/IRHelpers.h"

#include "lib/DataLayoutAnalysis/Backend/DLAMakeModelTypes.h"
#include "lib/DataLayoutAnalysis/Frontend/DLATypeSystemBuilder.h"
#include "lib/DataLayoutAnalysis/Middleend/DLAStep.h"
#include "lib/DataLayoutAnalysis/Middleend/FieldSizeComputation.h"
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

/// ArrangeAccessesHierarchically before it pushed each edge through another
/// once, verbatim except for this ordering, which has to be in
/// namespace dla to be found by the containers keyed by edge iterators
namespace dla {

// Helper ordering for NeighborIterators. We need it here because we need to use
// such iterators and keys in associative containers.
// Notice that this might have undefined behaviour if dereferencing either LHS
// or RHS is undefined behaviour itself.
// The bottom line is that we should never insert invalid iterators into
// associative containers.
static std::weak_ordering
operator<=>(const LayoutTypeSystem::NeighborIterator &LHS,
            const LayoutTypeSystem::NeighborIterator &RHS) {
  const auto &[LHSSucc, LHSTag] = *LHS;
  const auto &[RHSSucc, RHSTag] = *RHS;
  if (auto Cmp = LHSSucc->ID <=> RHSSucc->ID; Cmp != 0)
    return Cmp;

  if (nullptr == LHSTag or nullptr == RHSTag)
    return LHSTag <=> RHSTag;

  return *LHSTag <=> *RHSTag;
}

} // namespace dla

namespace reference_arrange {

static Logger Log{ "sort-accesses-hierarchically-reference" };


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
  if (Child->NonScalar or isLeaf(Child) or hasPointerChild(Child)
      or hasPointerParent(Child) or hasStridedParent(Child)
      or hasAnotherParent(Child, Parent)) {
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

static llvm::SmallPtrSet<LayoutTypeSystemNode *, 8>
absorbVolatileChildren(LayoutTypeSystem &TS, LayoutTypeSystemNode *Parent) {
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
      TS.dropOutgoingEdges(Volatile);
      revng_log(Log, "Merge volatile child: " << Volatile->ID);
      TS.mergeNodes({ Parent, Volatile });
    }

    for (auto &[OffsetExpr, Target] : CompoundEdges.takeVector())
      TS.addInstanceLink(Parent, Target, std::move(OffsetExpr));
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

/// \p AnalyzedTwice, when given, is set if some node is analyzed more than
/// once.
static bool arrangeAccessesHierarchically(LayoutTypeSystem &TS,
                                          bool *AnalyzedTwice = nullptr) {
  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());
  llvm::DenseSet<const LayoutTypeSystemNode *> Analyzed;

  bool Changed = false;

  absorbVolatileChildren(TS);

  // This dla::Step will heavily mutate the graph while iterating on it.
  // Despite thinking hard about it, we couldn't come up with a sane visit order
  // that could be pre-computed and guaranteed to be stable during the mutation,
  // or could be updated cheaply.
  // So we precompute some kind of global topological ordering, and use it as a
  // hard-coded ordering for analysis throughout the dla::Step.
  // Then we go fixed-point until we don't have anything left to recompute, but
  // we always follow this order because it's still better than iterating
  // randomly, or with some other ordering that is not stable.

  // Add a fake root to be able to compute a RPOT from that. What's important
  // here is not that it's strictly a RPOT, but that it is a topological
  // traversal.
  auto *FakeRoot = TS.createArtificialLayoutType();
  for (LayoutTypeSystemNode *Root : llvm::nodes(&TS)) {
    revng_assert(Root != nullptr);
    if (not isRoot(Root))
      continue;

    TS.addInstanceLink(FakeRoot, Root, OffsetExpression{});
  }

  // Compute the RPOT
  auto Queue = llvm::ReversePostOrderTraversal(NonPointerFilterT(FakeRoot));

  // Compute the node to analyze.
  std::set<const LayoutTypeSystemNode *> ToAnalyze;
  for (const auto *Node : Queue)
    ToAnalyze.insert(Node);

  // The fake root is never to analyze and can already be removed.
  ToAnalyze.erase(FakeRoot);
  TS.removeNode(FakeRoot);

  while (not ToAnalyze.empty()) {

    for (LayoutTypeSystemNode *Parent : Queue) {

      // Erase Parent from ToAnalyze
      bool AlreadyAnalyzed = not ToAnalyze.erase(Parent);

      // If erasure failed, Parent wasn't in ToAnalyze, so it was already
      // analyzed in a previous iteration, and we can skip it.
      if (AlreadyAnalyzed)
        continue;

      if (not Analyzed.insert(Parent).second and AnalyzedTwice != nullptr)
        *AnalyzedTwice = true;

      revng_log(Log, "Analyzing parent: " << Parent->ID);
      revng_assert(Parent != FakeRoot);
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
        auto Absorbed = absorbVolatileChildren(TS, Parent);
        for (auto *A : Absorbed)
          ToAnalyze.erase(A);
      }

      // Represents an edge iterator pointing to an instance edge, along with an
      // OffsetExpression that is not the one currently attached to the instance
      // edge, but that it is used to move the edge around.
      struct EdgeWithOffset {
        NeighborIterator Edge;
        OffsetExpression FinalOE;
      };

      revng_log(Log, "Initialize ChildrenHierarchy");
      // A map whose key is an edge iterator representing an instance edge, and
      // the mapped type is a vector of other edges that can be pushed through
      // the key, along the updated offsets that they will have after pushing
      // them through it.
      // In particular, we only keep in this map the neighbor iterator that are
      // the roots of the hierarchy of instance edges, i.e. the instance edges
      // that cannot be pushed through any other edge.
      std::map<NeighborIterator, llvm::SmallVector<EdgeWithOffset>>
        ChildrenHierarchy;

      using GT = llvm::GraphTraits<LayoutTypeSystemNode *>;

      // Initialize the ChildrenHierarchy.
      // At the beginning, all children edges are roots, and none of them has
      // other edges that can be pushed through them.
      for (auto AIt = GT::child_edge_begin(Parent),
                ChildEnd = GT::child_edge_end(Parent);
           AIt != ChildEnd;
           ++AIt) {
        if (isInstanceEdge(*AIt))
          ChildrenHierarchy[AIt];
      }

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
        // should be pushed through it, along with the new offsets they will
        // have after the push through.
        llvm::SmallVector<EdgeWithOffset> ContainedInA;

        // A vector of instance edges that contain AEdgeIt. AEdgeIt will have to
        // be pushed through all of them. The FinalOE in EdgeWithOffset here
        // represents the new offset that AEdgeIt will have after being pushed
        // through them.
        llvm::SmallVector<EdgeWithOffset> ContainsA;

        for (auto BRootIt = ARootNext; BRootIt != HierarchyEnd; ++BRootIt) {
          LoggerIndent OtherChildIndent{ Log };

          auto &[BEdgeIt, ContainedInB] = *BRootIt;
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
            auto BWithNewAOffset = EdgeWithOffset{ BEdgeIt,
                                                   std::move(OEAfterPush) };
            ContainsA.push_back(std::move(BWithNewAOffset));
          } else {
            revng_assert(ToPush == BEdgeIt and Through == AEdgeIt);
            // BEdgeIt can be pushed inside AEdgeIt
            auto BWithNewBOffset = EdgeWithOffset{ BEdgeIt,
                                                   std::move(OEAfterPush) };
            ContainedInA.push_back(std::move(BWithNewBOffset));
          }
        }

        // Now, push all the roots contained in A into A, and remove them from
        // ChildrenHierarchy, since they're not roots anymore.
        PushedInsideA.reserve(PushedInsideA.size() + ContainedInA.size());
        PushedInsideA.append(ContainedInA);
        for (const auto &[PushedInA, Unused] : ContainedInA)
          ChildrenHierarchy.erase(PushedInA);

        // Then, for all the other edges that contain A, A can be pushed through
        // them, along with all the nodes that have already been pushed through
        // A itself (that transitively can be pushed through the others too).
        // After we've pushed A through all edges in ContainsA, we can remove A
        // itelf from ChildrenHierarchy, since it's not a root anymore.
        ARootNext = std::next(ARootIt);
        if (not ContainsA.empty()) {
          for (const auto &[LargerThanA, FinalOE] : ContainsA) {

            // If A needs to be pushed through LargerThanA, and all things that
            // were previously pushed pushed through A can now be pushed through
            // LargerThanA.
            auto &PushedThroughLargerThanA = ChildrenHierarchy.at(LargerThanA);
            PushedThroughLargerThanA.reserve(PushedThroughLargerThanA.size()
                                             + PushedInsideA.size() + 1);
            for (auto &[EdgeToPush, OEAfterPush] : PushedInsideA) {
              // We have to recompute the offset of EdgeToPush here, after being
              // pushed through LargerThanA, because this value was never
              // computed before, given that these two edges have never been
              // compared before.
              PushedThroughLargerThanA.push_back(EdgeWithOffset{
                EdgeToPush, computeOffsetAfterPush(EdgeToPush, LargerThanA) });
            }
            // Then also AEdgeIt can be pushed through LargerThanA. The final
            // offset of AEdgeIt after being pushed through has already been
            // computed in advance, so we just use that.
            PushedThroughLargerThanA.push_back({ AEdgeIt, std::move(FinalOE) });
          }
          // Now AEdgeIt is not a root anymore, so we have to erase it and
          // update ARootNext.
          ARootNext = ChildrenHierarchy.erase(ARootIt);
        }
      }

      llvm::SmallSet<NeighborIterator, 4> EdgesToErase;
      {
        revng_log(Log, "Collect EdgesToErase");
        LoggerIndent IndentCollect{ Log };
        for (auto &[EdgeToPushThrough, EdgesToPush] : ChildrenHierarchy) {

          if (EdgesToPush.empty())
            continue;

          auto *ToPushThrough = EdgeToPushThrough->first;
          ToAnalyze.insert(ToPushThrough);

          for (auto &[PushedEdgeIt, FinalOE] : EdgesToPush) {
            auto *ToPushDown = PushedEdgeIt->first;

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
          TS.eraseEdge(Parent, ToErase);
        }
      }

      Changed |= not EdgesToErase.empty();
    }
  }

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  return Changed;
}

} // namespace reference_arrange

/// A random DAG of nested fields: each node has a size, and its instance
/// children fit in it at random offsets, some of them strided, so that many
/// children of a node contain one another. One node in eight is a pointer
/// leaf pointing to an earlier node.
static void buildNestedFields(LayoutTypeSystem &TS,
                              unsigned Seed,
                              size_t NodeCount,
                              size_t ChildrenPerNode) {
  std::mt19937 Generator(Seed);
  const uint64_t Sizes[] = { 256, 128, 64, 32, 16, 8, 4, 2, 1 };
  std::vector<LTSN *> Nodes;
  for (size_t I = 0; I < NodeCount; ++I) {
    LTSN *N = TS.createArtificialLayoutType();
    // Sizes shrink along the order, so that later nodes fit in earlier ones
    size_t Level = (I * 9) / NodeCount;
    size_t Pick = std::min<size_t>(8,
                                   Level
                                     + std::uniform_int_distribution<size_t>(
                                       0, 2)(Generator));
    N->Size = Sizes[Pick];
    Nodes.push_back(N);
  }

  for (size_t I = 0; I < NodeCount; ++I) {
    LTSN *Parent = Nodes[I];
    if (I % 8 == 7) {
      if (I > 0)
        TS.addPointerLink(Parent,
                          Nodes[std::uniform_int_distribution<size_t>(
                            0, I - 1)(Generator)]);
      Parent->Size = 8;
      continue;
    }
    for (size_t C = 0; C < ChildrenPerNode and I + 1 < NodeCount; ++C) {
      size_t J = std::uniform_int_distribution<size_t>(I + 1, NodeCount - 1)(
        Generator);
      LTSN *Child = Nodes[J];
      if (Child->Size > Parent->Size)
        continue;
      uint64_t Room = Parent->Size - Child->Size;
      bool Aligned = std::bernoulli_distribution(0.8)(Generator);
      uint64_t Offset = std::uniform_int_distribution<uint64_t>(0, Room)(
        Generator);
      if (Aligned)
        Offset -= Offset % Child->Size;
      OffsetExpression OE{ Offset };
      uint64_t Count = (Parent->Size - Offset) / Child->Size;
      if (Count > 1 and std::bernoulli_distribution(0.15)(Generator)) {
        OE.Strides.push_back(Child->Size);
        OE.TripCounts.push_back(Count);
      }
      TS.addInstanceLink(Parent, Child, std::move(OE));
    }
  }
}

/// One parent holding fields of every size, each containing the next smaller
/// one at offset 0, plus a leaf: every field can be pushed through all the
/// larger ones, the shape whose pushed edge lists grew exponentially
static void buildContainedFields(LayoutTypeSystem &TS, size_t Depth) {
  LTSN *Parent = TS.createArtificialLayoutType();
  Parent->Size = uint64_t(1) << Depth;
  LTSN *Previous = nullptr;
  for (size_t Level = Depth; Level-- > 0;) {
    LTSN *Field = TS.createArtificialLayoutType();
    Field->Size = uint64_t(1) << Level;
    TS.addInstanceLink(Parent, Field, OffsetExpression{});
    LTSN *Leaf = TS.createArtificialLayoutType();
    Leaf->Size = Field->Size;
    TS.addInstanceLink(Field, Leaf, OffsetExpression{});
    if (Previous != nullptr)
      TS.addInstanceLink(Previous, Field, OffsetExpression{});
    Previous = Field;
  }
}

/// Each root with each leaf it reaches through non-pointer edges, by node ID
static std::set<std::pair<uint64_t, uint64_t>>
leavesReachableFromRoots(LayoutTypeSystem &TS) {
  std::set<std::pair<uint64_t, uint64_t>> Result;
  for (LTSN *Root : llvm::nodes(&TS)) {
    if (not isRoot(Root))
      continue;
    using reference_arrange::NonPointerFilterT;
    for (LTSN *N : llvm::depth_first(NonPointerFilterT(Root)))
      if (isLeaf(N))
        Result.insert({ Root->ID, N->ID });
  }
  return Result;
}

/// No node is left with two instance children of which one can still be
/// pushed through the other, except where the push would close an instance
/// loop
static bool isArranged(LayoutTypeSystem &TS) {
  using GT = llvm::GraphTraits<LTSN *>;
  for (LTSN *Parent : llvm::nodes(&TS)) {
    for (auto AIt = GT::child_edge_begin(Parent),
              End = GT::child_edge_end(Parent);
         AIt != End;
         ++AIt) {
      for (auto BIt = std::next(AIt); BIt != End; ++BIt) {
        auto Push = reference_arrange::canPushThrough(Parent, AIt, BIt);
        if (not Push.has_value())
          continue;
        auto *Through = Push->Through->first;
        auto *Pushed = Push->ToPush->first;
        uint64_t PushedSize = getFieldSize(Pushed, Push->ToPush->second);
        if (Push->OEAfterPush.Offset == 0 and PushedSize == Through->Size
            and reference_arrange::wouldCloseInstanceLoop(Parent,
                                                          Through,
                                                          Pushed))
          continue;
        return false;
      }
    }
  }
  return true;
}

template<typename BuilderT>
static void prepareForArrange(LayoutTypeSystem &TS, BuilderT Build) {
  Build(TS);
  dla::StepManager SM;
  revng_check(SM.addStep<CollapseEqualitySCC>());
  revng_check(SM.addStep<CollapseInstanceAtOffset0SCC>());
  revng_check(SM.addStep<PruneLayoutNodesWithoutLayout>());
  revng_check(SM.addStep<ComputeUpperMemberAccesses>());
  revng_check(SM.addStep<DecomposeStridedEdges>());
  SM.run(TS);
}

struct ArrangeComparison {
  bool ReferenceAnalyzedTwice = false;
  bool Equal = false;
};

/// Run the step and the previous implementation on two copies of the same
/// graph; the step must leave every node arranged, keep every leaf reachable
/// from the roots that reached it, and give the same graph as the previous
/// implementation whenever that one analyzed no node twice
template<typename BuilderT>
static ArrangeComparison checkArrangeAgainstReference(BuilderT Build) {
  VerifyLog.enable();
  LayoutTypeSystem Reference, Actual;
  prepareForArrange(Reference, Build);
  prepareForArrange(Actual, Build);
  auto LeavesBefore = leavesReachableFromRoots(Actual);

  ArrangeComparison Result;
  bool ReferenceChanged = reference_arrange::arrangeAccessesHierarchically(
    Reference, &Result.ReferenceAnalyzedTwice);
  bool ActualChanged = ArrangeAccessesHierarchically().runOnTypeSystem(Actual);

  revng_check(isArranged(Actual));
  auto LeavesAfter = leavesReachableFromRoots(Actual);
  revng_check(std::includes(LeavesAfter.begin(),
                            LeavesAfter.end(),
                            LeavesBefore.begin(),
                            LeavesBefore.end()));

  Result.Equal = describe(Reference) == describe(Actual);
  if (not Result.ReferenceAnalyzedTwice) {
    revng_check(ReferenceChanged == ActualChanged);
    revng_check(Result.Equal);
  }
  return Result;
}

BOOST_AUTO_TEST_CASE(ArrangeAccessesHierarchicallyMatchesReferenceVisitingOnce) {
  for (size_t Depth : { 1, 2, 3, 6, 9 })
    checkArrangeAgainstReference([&](LayoutTypeSystem &TS) {
      buildContainedFields(TS, Depth);
    });

  unsigned VisitedOnce = 0;
  for (unsigned Seed = 0; Seed < 600; ++Seed) {
    size_t NodeCount = 2 + Seed % 71;
    size_t ChildrenPerNode = 1 + Seed % 6;
    auto Comparison = checkArrangeAgainstReference([&](LayoutTypeSystem &TS) {
      buildNestedFields(TS, Seed, NodeCount, ChildrenPerNode);
    });
    VisitedOnce += not Comparison.ReferenceAnalyzedTwice;
  }
  // The previous implementation analyzes no node twice in 264 of these graphs,
  // so the equality above is checked on them
  revng_check(VisitedOnce == 264);
}


// Equal storage extent does not make a pointer hypothesis subsume a scalar.
BOOST_AUTO_TEST_CASE(ResolveLeafUnionsPreservesScalarAlternatives) {
  for (bool PointerFirst : { false, true }) {
    LayoutTypeSystem TS;
    auto *Root = createRoot(TS, 16);
    auto *First = addInstanceAtOffset(TS, Root, 0, 8);
    auto *Second = addInstanceAtOffset(TS, Root, 0, 8);
    auto *Scalar = PointerFirst ? Second : First;
    auto *Pointer = PointerFirst ? First : Second;
    TS.addPointerLink(Pointer, createRoot(TS, 4));
    auto *OtherOffset = addInstanceAtOffset(TS, Root, 8, 8);

    runStep<ResolveLeafUnions>(TS);

    BOOST_CHECK_EQUAL(countInstanceEdges(Root, Scalar), 1);
    BOOST_CHECK_EQUAL(countInstanceEdges(Root, Pointer), 1);
    BOOST_CHECK_EQUAL(countInstanceEdges(Root, OtherOffset), 1);
    BOOST_CHECK(isPointerNode(Pointer));
    BOOST_CHECK(not isPointerNode(Scalar));
  }
}

BOOST_AUTO_TEST_CASE(ResolveLeafUnionsStillSimplifiesContainedPointees) {
  LayoutTypeSystem TS;
  auto *Root = createRoot(TS, 8);
  auto *General = addInstanceAtOffset(TS, Root, 0, 8);
  auto *Specific = addInstanceAtOffset(TS, Root, 0, 8);
  auto *Pointee = createRoot(TS, 16);
  auto *Contained = addInstanceAtOffset(TS, Pointee, 0, 8);
  TS.addPointerLink(General, Pointee);
  TS.addPointerLink(Specific, Contained);

  runStep<ResolveLeafUnions>(TS);

  BOOST_CHECK_EQUAL(countInstanceEdges(Root, General), 1);
  BOOST_CHECK_EQUAL(countInstanceEdges(Root, Specific), 0);
}

BOOST_AUTO_TEST_CASE(PruningRetainsPointerAlternativeWithoutChangingLayout) {
  for (bool FromModel : { false, true }) {
    LayoutTypeSystem TS;
    auto *Root = createRoot(TS, 16);
    addInstanceAtOffset(TS, Root, 0, 8);
    auto *Access = addInstanceAtOffset(TS, Root, 8, 8);
    Access->NonScalar = FromModel;
    auto *Unknown = createRoot(TS);
    unsigned UnknownID = Unknown->ID;
    TS.addPointerLink(Access, Unknown);

    PruneLayoutNodesWithoutLayout Prune;
    BOOST_CHECK(Prune.runOnTypeSystem(TS));
    BOOST_CHECK(TS.getEqClasses().isRemoved(UnknownID));
    BOOST_CHECK(not Prune.runOnTypeSystem(TS));
    BOOST_CHECK(TS.verifyConsistency() and TS.verifyLeafs());
    BOOST_REQUIRE(Access->Successors.empty());
    BOOST_CHECK(Access->HasUnknownPointee);
    ComputeUpperMemberAccesses().runOnTypeSystem(TS);
    SimplifyInstanceAtOffset0().runOnTypeSystem(TS);
    DeduplicateFields().runOnTypeSystem(TS);
    PushDownPointers().runOnTypeSystem(TS);
    ResolveLeafUnions().runOnTypeSystem(TS);
    ComputeNonInterferingComponents().runOnTypeSystem(TS);
    TS.getEqClasses().compress();

    llvm::LLVMContext Context;
    llvm::Argument Argument(llvm::Type::getInt64Ty(Context));
    LayoutTypePtrVect Values{ LayoutTypePtr(&Argument) };
    TupleTree<model::Binary> Model;
    Model->Architecture() = model::Architecture::x86_64;
    auto Types = makeModelTypes(TS, Values, Model);
    auto *Struct = llvm::dyn_cast<model::StructDefinition>(
      Types.at(Values[0])->tryGetAsDefinition());
    BOOST_REQUIRE(Struct);
    BOOST_CHECK_EQUAL(Struct->Size(), 16);
    bool Capacity = false;
    bool Allocation = false;
    for (const auto &Field : Struct->Fields()) {
      if (Field.Offset() == 0) {
        Capacity = llvm::isa<model::PrimitiveType>(Field.Type().get());
        BOOST_CHECK_EQUAL(*Field.Type()->size(), 8);
      } else if (Field.Offset() == 8) {
        auto *Union = llvm::dyn_cast<model::UnionDefinition>(
          Field.Type()->tryGetAsDefinition());
        BOOST_REQUIRE(Union);
        bool Scalar = false;
        for (const auto &Alternative : Union->Fields()) {
          const auto *Type = Alternative.Type().get();
          Scalar |= llvm::isa<model::PrimitiveType>(Type)
                    or (FromModel and Type->tryGetAsDefinition());
          if (auto *Pointer = llvm::dyn_cast<model::PointerType>(Type)) {
            auto *Pointee = llvm::dyn_cast<model::PrimitiveType>(
              Pointer->PointeeType().get());
            BOOST_REQUIRE(Pointee);
            BOOST_CHECK(Pointee->PrimitiveKind() == model::PrimitiveKind::Void);
            BOOST_CHECK_EQUAL(Pointer->PointerSize(), 8);
            Allocation = true;
          }
        }
        BOOST_CHECK(Scalar);
      }
    }
    BOOST_CHECK(Capacity and Allocation);
    BOOST_CHECK(Model->verify());
  }
}

BOOST_AUTO_TEST_CASE(PruningKeepsKnownPointees) {
  LayoutTypeSystem TS;
  auto *Root = createRoot(TS, 8);
  auto *Pointer = addInstanceAtOffset(TS, Root, 0, 8);
  auto *Known = createRoot(TS, 4);
  TS.addPointerLink(Pointer, Known);
  BOOST_CHECK(not PruneLayoutNodesWithoutLayout().runOnTypeSystem(TS));
  BOOST_CHECK(not Pointer->HasUnknownPointee);
  BOOST_REQUIRE_EQUAL(Pointer->Successors.size(), 1);
  BOOST_CHECK(Pointer->Successors.begin()->first == Known);
}

BOOST_AUTO_TEST_CASE(StackFrameClippingPreservesSharedUnion) {
  TupleTree<model::Binary> Model;
  Model->Architecture() = model::Architecture::x86_64;
  auto &&[Large, LargeType] = Model->makeStructDefinition();
  Large.Size() = 24;
  Large.addField(0, model::PrimitiveType::make(model::PrimitiveKind::Generic, 8));
  auto &&[Shared, SharedType] = Model->makeUnionDefinition();
  Shared.addField(LargeType.copy());
  Shared.addField(model::PointerType::make(model::PrimitiveType::makeVoid(),
                                         Model->Architecture()));
  auto &&[Stack, StackType] = Model->makeStructDefinition();
  Stack.Size() = 8;
  auto &&[Prototype, PrototypeType] = Model->makeRawFunctionDefinition();
  Prototype.Architecture() = Model->Architecture();
  Prototype.FinalStackOffset() = 8;
  Prototype.addArgument(model::Register::rdi_x86_64,
                        model::PrimitiveType::make(model::PrimitiveKind::Generic,
                                                   8));
  auto Entry = MetaAddress::fromPC(Model->Architecture(), 0x1000);
  model::Segment Segment(MetaAddress::fromGeneric(Model->Architecture(),
                                                   0x1000));
  Segment.VirtualSize() = 0x100;
  Segment.FileSize() = 0x100;
  Segment.IsExecutable() = true;
  Model->Segments().insert(std::move(Segment));
  auto &Function = Model->Functions()[Entry];
  Function.Prototype() = PrototypeType.copy();
  Function.StackFrame().Type() = StackType.copy();

  llvm::LLVMContext Context;
  llvm::Module Module("shared-union", Context);
  auto *FunctionType = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                              { llvm::Type::getInt64Ty(Context) },
                                              false);
  auto *LLVMFunction = llvm::Function::Create(FunctionType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "shared_union",
                                             Module);
  setMetaAddressMetadata(LLVMFunction, FunctionEntryMDName, Entry);
  auto *Block = llvm::BasicBlock::Create(Context, "entry", LLVMFunction);
  llvm::IRBuilder<> Builder(Block);
  auto *Allocation = Builder.CreateAlloca(Builder.getInt64Ty());
  setStackFrameMetadata(Allocation);
  Builder.CreateRetVoid();
  TypeMapT Types;
  Types.emplace(LayoutTypePtr(LLVMFunction->getArg(0)), SharedType.copy());
  Types.emplace(LayoutTypePtr(Allocation), SharedType.copy());

  BOOST_REQUIRE(Model->verify(true));
  BOOST_CHECK(updateFuncSignatures(Module, Model, Types));
  BOOST_CHECK_EQUAL(Shared.Fields().size(), 2);
  BOOST_CHECK_EQUAL(*Shared.size(), 24);
  BOOST_REQUIRE_EQUAL(Stack.Fields().size(), 1);
  BOOST_CHECK_EQUAL(*Stack.Fields().begin()->Type()->size(), 8);
  BOOST_CHECK(Model->verify());
}

BOOST_AUTO_TEST_CASE(PointeeUnionPreservesContainedAggregateViews) {
  for (bool Indirect : { false, true }) {
    LayoutTypeSystem TS;
    auto *Union = createRoot(TS, 8);
    auto *OuterPointer = addInstanceAtOffset(TS, Union, 0, 8);
    auto *InnerPointer = addInstanceAtOffset(TS, Union, 0, 8);
    auto *Outer = createRoot(TS, 320);
    auto *Inner = createRoot(TS, 16);
    auto *Middle = Indirect ? createRoot(TS, 24) : Outer;
    if (Indirect)
      TS.addInstanceLink(Outer, Middle, OffsetExpression{ 0 });
    TS.addInstanceLink(Middle, Inner, OffsetExpression{ 0 });
    addInstanceAtOffset(TS, Inner, 0, 8);
    addInstanceAtOffset(TS, Inner, 8, 8);
    addInstanceAtOffset(TS, Outer, 312, 8);
    TS.addPointerLink(OuterPointer, Outer);
    TS.addPointerLink(InnerPointer, Inner);
    auto *Consumer = createRoot(TS, 24);
    TS.addInstanceLink(Consumer, Inner, OffsetExpression{ 0 });
    addInstanceAtOffset(TS, Consumer, 16, 8);

    MergePointeesOfPointerUnion Step(8);
    Step.runOnTypeSystem(TS);
    BOOST_CHECK(TS.verifyDAG());
    BOOST_REQUIRE(llvm::is_contained(TS.getLayoutsRange(), Inner));
    BOOST_CHECK_EQUAL(Inner->Size, 16);
    BOOST_CHECK_EQUAL(Consumer->Size, 24);
    BOOST_CHECK_EQUAL(countInstanceEdges(Consumer, Inner), 1);
    BOOST_CHECK_EQUAL(InnerPointer->Successors.begin()->first, Inner);
  }
}

BOOST_AUTO_TEST_CASE(PointeeUnionStillMergesIndependentAggregateViews) {
  LayoutTypeSystem TS;
  auto *Union = createRoot(TS, 8);
  auto *FirstPointer = addInstanceAtOffset(TS, Union, 0, 8);
  auto *SecondPointer = addInstanceAtOffset(TS, Union, 0, 8);
  auto *First = createRoot(TS, 16);
  auto *Second = createRoot(TS, 24);
  addInstanceAtOffset(TS, First, 0, 8);
  addInstanceAtOffset(TS, Second, 16, 8);
  TS.addPointerLink(FirstPointer, First);
  TS.addPointerLink(SecondPointer, Second);

  MergePointeesOfPointerUnion Step(8);
  Step.runOnTypeSystem(TS);
  BOOST_CHECK(TS.verifyDAG());
  BOOST_CHECK_EQUAL(FirstPointer->Successors.begin()->first,
                    SecondPointer->Successors.begin()->first);
  BOOST_CHECK_EQUAL(Second->Size, 24);
  BOOST_CHECK_EQUAL(Second->Successors.size(), 2);
}

BOOST_AUTO_TEST_CASE(FunctionIdentitySeparatesSharedPrototypeLayouts) {
  TupleTree<model::Binary> Model;
  Model->Architecture() = model::Architecture::x86_64;
  auto Generic = model::PrimitiveType::make(model::PrimitiveKind::Generic, 8);
  auto &&[Prototype, PrototypeType] = Model->makeRawFunctionDefinition();
  Prototype.Architecture() = Model->Architecture();
  Prototype.FinalStackOffset() = 8;
  Prototype.addArgument(model::Register::rdi_x86_64, Generic.copy());
  Prototype.addReturnValue(model::Register::rax_x86_64, Generic.copy());
  model::Segment Segment(MetaAddress::fromGeneric(Model->Architecture(),
                                                  0x1000));
  Segment.VirtualSize() = 0x100;
  Segment.FileSize() = 0x100;
  Segment.IsExecutable() = true;
  Model->Segments().insert(std::move(Segment));
  llvm::LLVMContext Context;
  llvm::Module FirstModule("first", Context), SecondModule("second", Context);
  FirstModule.setDataLayout("e-p:64:64");
  SecondModule.setDataLayout("e-p:64:64");
  auto *Integer = llvm::Type::getInt64Ty(Context);
  auto *Type = llvm::FunctionType::get(Integer, { Integer }, false);
  auto Declare = [&](llvm::Module &Module, llvm::StringRef Name, uint64_t PC) {
    auto Entry = MetaAddress::fromPC(Model->Architecture(), PC);
    Model->Functions()[Entry].Prototype() = PrototypeType.copy();
    auto *Function = llvm::Function::Create(Type,
                                            llvm::GlobalValue::ExternalLinkage,
                                            Name,
                                            Module);
    FunctionTags::Isolated.addTo(Function);
    setMetaAddressMetadata(Function, FunctionEntryMDName, Entry);
    return Function;
  };
  auto *First = Declare(FirstModule, "first_function", 0x1000);
  auto *Other = Declare(FirstModule, "other_function", 0x1010);
  auto *Copy = Declare(SecondModule, "first_function", 0x1000);
  auto *Caller = Declare(SecondModule, "caller", 0x1020);
  auto *Block = llvm::BasicBlock::Create(Context, "entry", Caller);
  llvm::IRBuilder<> IR(Block);
  auto Annotate = [&](llvm::CallInst *Call) {
    setStringMetadata(Call,
                      PrototypeMDName,
                      Model->getTypeDefinitionReference(Prototype.key())
                        .toString());
    return Call;
  };
  auto *Direct = Annotate(IR.CreateCall(Copy, { Caller->getArg(0) }));
  auto *Target = IR.CreateIntToPtr(Caller->getArg(0), IR.getPtrTy());
  auto *Indirect = Annotate(IR.CreateCall(Type, Target, { IR.getInt64(8) }));
  IR.CreateRet(Direct);
  BOOST_REQUIRE(Model->verify(true));

  LayoutTypeSystem TS;
  DLATypeSystemLLVMBuilder Builder(TS, *Model);
  std::vector<llvm::Module *> Modules{ &FirstModule, &SecondModule };
  Builder.buildFromLLVMModules(Modules, { "first", "second" });
  auto ID = [&](llvm::Value *Value) {
    auto &Values = Builder.getValues();
    auto It = llvm::find(Values, LayoutTypePtr(Value));
    revng_assert(It != Values.end());
    return static_cast<unsigned>(std::distance(Values.begin(), It));
  };
  for (auto *Node : TS.getLayoutsRange())
    if (Node->ID == ID(First->getArg(0)) or Node->ID == ID(First))
      BOOST_CHECK(Node->IsTypeRoot);
  unsigned FirstArg = ID(First->getArg(0)), OtherArg = ID(Other->getArg(0));
  unsigned CopyArg = ID(Copy->getArg(0)), Constant = ID(IR.getInt64(8));
  unsigned FirstReturn = ID(First), OtherReturn = ID(Other);
  unsigned CopyReturn = ID(Copy), DirectReturn = ID(Direct);
  unsigned IndirectReturn = ID(Indirect);
  runStep<CollapseEqualitySCC>(TS);
  auto &Eq = TS.getEqClasses();
  BOOST_CHECK(Eq.haveSameEqClass(FirstArg, CopyArg));
  BOOST_CHECK(not Eq.haveSameEqClass(FirstArg, OtherArg));
  BOOST_CHECK(not Eq.haveSameEqClass(FirstArg, Constant));
  BOOST_CHECK(Eq.haveSameEqClass(FirstReturn, CopyReturn));
  BOOST_CHECK(Eq.haveSameEqClass(FirstReturn, DirectReturn));
  BOOST_CHECK(not Eq.haveSameEqClass(FirstReturn, OtherReturn));
  BOOST_CHECK(not Eq.haveSameEqClass(FirstReturn, IndirectReturn));
}

BOOST_AUTO_TEST_CASE(StructuralSimplificationPreservesFunctionSubobjectLayout) {
  for (bool Arrange : { false, true }) {
    LayoutTypeSystem TS;
    auto *Parent = TS.createArtificialLayoutType();
    auto *Argument = TS.createArtificialLayoutType();
    auto *Low = TS.createArtificialLayoutType();
    auto *High = TS.createArtificialLayoutType();
    auto *Tail = TS.createArtificialLayoutType();
    Argument->IsTypeRoot = true;
    Low->Size = High->Size = 4;
    Tail->Size = 8;
    TS.addInstanceLink(Parent, Argument, OffsetExpression{ 0 });
    TS.addInstanceLink(Parent, Tail, OffsetExpression{ 8 });
    TS.addInstanceLink(Argument, Low, OffsetExpression{ 0 });
    TS.addInstanceLink(Argument, High, OffsetExpression{ 4 });
    unsigned ArgumentID = Argument->ID, ParentID = Parent->ID;
    if (Arrange) {
      ComputeUpperMemberAccesses().runOnTypeSystem(TS);
      ArrangeAccessesHierarchically().runOnTypeSystem(TS);
    } else {
      SimplifyInstanceAtOffset0().runOnTypeSystem(TS);
      ComputeUpperMemberAccesses().runOnTypeSystem(TS);
    }
    bool Distinct = not TS.getEqClasses().haveSameEqClass(ArgumentID, ParentID);
    BOOST_CHECK(Distinct);
    if (Distinct) {
      BOOST_CHECK_EQUAL(Argument->Size, 8);
      BOOST_CHECK_EQUAL(Parent->Size, 16);
    }
    BOOST_CHECK(TS.verifyDAG());
  }
}

BOOST_AUTO_TEST_CASE(SingleChildPreservesFunctionArgumentOrigin) {
  LayoutTypeSystem TS;
  auto *Argument = TS.createArtificialLayoutType();
  auto *Caller = TS.createArtificialLayoutType();
  auto *Access = TS.createArtificialLayoutType();
  Caller->Size = Argument->Size = 16;
  Argument->IsTypeRoot = true;
  Access->Size = 8;
  TS.addInstanceLink(Caller, Argument, OffsetExpression{ 0 });
  TS.addInstanceLink(Argument, Access, OffsetExpression{ 8 });
  unsigned ArgumentID = Argument->ID, AccessID = Access->ID;
  CollapseSingleChild().runOnTypeSystem(TS);
  BOOST_CHECK(not TS.getEqClasses().haveSameEqClass(ArgumentID, AccessID));
  BOOST_CHECK_EQUAL(Caller->Size, 16);
  BOOST_CHECK(Caller->IsTypeRoot);
}

BOOST_AUTO_TEST_CASE(PointeeUnionPreservesFunctionLayoutAlternatives) {
  for (bool MarkSmaller : { false, true }) {
    LayoutTypeSystem TS;
    auto *Union = createRoot(TS, 8);
    auto *FirstPointer = addInstanceAtOffset(TS, Union, 0, 8);
    auto *SecondPointer = addInstanceAtOffset(TS, Union, 0, 8);
    auto *First = createRoot(TS, 16);
    auto *Second = createRoot(TS, 24);
    (MarkSmaller ? First : Second)->IsTypeRoot = true;
    addInstanceAtOffset(TS, First, 0, 8);
    addInstanceAtOffset(TS, First, 8, 8);
    addInstanceAtOffset(TS, Second, 16, 8);
    TS.addPointerLink(FirstPointer, First);
    TS.addPointerLink(SecondPointer, Second);
    unsigned FirstID = First->ID, SecondID = Second->ID;
    MergePointeesOfPointerUnion(8).runOnTypeSystem(TS);
    BOOST_REQUIRE(not TS.getEqClasses().haveSameEqClass(FirstID, SecondID));
    BOOST_CHECK_EQUAL(First->Size, 16);
    BOOST_CHECK_EQUAL(Second->Size, 24);
    BOOST_CHECK_EQUAL(FirstPointer->Successors.begin()->first, First);
    BOOST_CHECK_EQUAL(SecondPointer->Successors.begin()->first, Second);
    BOOST_CHECK(TS.verifyDAG());
  }
}

BOOST_AUTO_TEST_CASE(DeduplicateCompatibleFunctionViews) {
  for (bool Reverse : { false, true }) {
    LayoutTypeSystem TS;
    auto *Object = createRoot(TS, 48);
    auto *Sparse = createRoot(TS, 24);
    auto *Complete = createRoot(TS, 24);
    Sparse->IsTypeRoot = Complete->IsTypeRoot = true;
    auto *Nested = addInstanceAtOffset(TS, Sparse, 0, 24);
    addInstanceAtOffset(TS, Nested, 8, 8);
    addInstanceAtOffset(TS, Nested, 16, 8)->HasUnknownPointee = true;
    addInstanceAtOffset(TS, Complete, 0, 8);
    addInstanceAtOffset(TS, Complete, 8, 8);
    addInstanceAtOffset(TS, Complete, 16, 8)->HasUnknownPointee = true;
    TS.addInstanceLink(Object,
                       Reverse ? Complete : Sparse,
                       OffsetExpression{ 16 });
    TS.addInstanceLink(Object,
                       Reverse ? Sparse : Complete,
                       OffsetExpression{ 16 });
    unsigned SparseID = Sparse->ID, CompleteID = Complete->ID;
    DeduplicateFields().runOnTypeSystem(TS);
    BOOST_CHECK(TS.getEqClasses().haveSameEqClass(SparseID, CompleteID));
    BOOST_CHECK(TS.verifyDAG());
  }
}

BOOST_AUTO_TEST_CASE(DeduplicateFunctionViewsRejectsDifferentStorage) {
  for (unsigned Case = 0; Case < 6; ++Case) {
    LayoutTypeSystem TS;
    auto *Object = createRoot(TS, 64);
    auto *Sparse = createRoot(TS, 24);
    auto *Complete = createRoot(TS, Case == 0 ? 32 : 24);
    Sparse->IsTypeRoot = Complete->IsTypeRoot = true;
    addInstanceAtOffset(TS, Sparse, 8, 8);
    addInstanceAtOffset(TS, Sparse, 16, 8);
    auto *Field = addInstanceAtOffset(TS, Complete, 8, Case == 1 ? 4 : 8);
    addInstanceAtOffset(TS, Complete, 0, 8);
    addInstanceAtOffset(TS, Complete, 16, 8);
    if (Case == 5)
      Field->HasUnknownPointee = true;
    if (Case == 2)
      TS.addPointerLink(Field, createRoot(TS, 8));
    TS.addInstanceLink(Object, Sparse, OffsetExpression{ 0 });
    if (Case == 3)
      Object = createRoot(TS, 64);
    TS.addInstanceLink(Object,
                       Complete,
                       OffsetExpression{ Case == 4 ? 8ULL : 0ULL });
    unsigned SparseID = Sparse->ID, CompleteID = Complete->ID;
    DeduplicateFields().runOnTypeSystem(TS);
    BOOST_CHECK(not TS.getEqClasses().haveSameEqClass(SparseID, CompleteID));
    BOOST_CHECK(TS.verifyDAG());
  }
}

BOOST_AUTO_TEST_CASE(DeduplicateFunctionViewsPreservesPointerAlternatives) {
  LayoutTypeSystem TS;
  auto *Object = createRoot(TS, 8);
  auto *First = addInstanceAtOffset(TS, Object, 0, 8);
  auto *Second = addInstanceAtOffset(TS, Object, 0, 8);
  First->IsTypeRoot = Second->IsTypeRoot = true;
  TS.addPointerLink(First, createRoot(TS, 16));
  TS.addPointerLink(Second, createRoot(TS, 24));
  unsigned FirstID = First->ID, SecondID = Second->ID;
  DeduplicateFields().runOnTypeSystem(TS);
  BOOST_REQUIRE(not TS.getEqClasses().haveSameEqClass(FirstID, SecondID));
  BOOST_CHECK_EQUAL(First->Successors.size(), 1);
  BOOST_CHECK_EQUAL(Second->Successors.size(), 1);
  BOOST_CHECK(TS.verifyDAG());
}

BOOST_AUTO_TEST_CASE(DeduplicateFunctionViewsPreservesContainment) {
  LayoutTypeSystem TS;
  auto *Object = createRoot(TS, 24);
  auto *Sparse = createRoot(TS, 24);
  auto *Complete = createRoot(TS, 24);
  Sparse->IsTypeRoot = Complete->IsTypeRoot = true;
  addInstanceAtOffset(TS, Sparse, 16, 8);
  addInstanceAtOffset(TS, Complete, 0, 8);
  TS.addInstanceLink(Complete, Sparse, OffsetExpression{ 0 });
  TS.addInstanceLink(Object, Sparse, OffsetExpression{ 0 });
  TS.addInstanceLink(Object, Complete, OffsetExpression{ 0 });
  unsigned SparseID = Sparse->ID, CompleteID = Complete->ID;
  DeduplicateFields().runOnTypeSystem(TS);
  BOOST_CHECK(not TS.getEqClasses().haveSameEqClass(SparseID, CompleteID));
  BOOST_CHECK(TS.verifyDAG());
}

BOOST_AUTO_TEST_CASE(ArrangeKeepsCrossFieldAccessOutsideFunctionView) {
  LayoutTypeSystem TS;
  auto *Object = createRoot(TS, 32);
  auto *View = addInstanceAtOffset(TS, Object, 0, 16);
  View->IsTypeRoot = true;
  addInstanceAtOffset(TS, View, 0, 8);
  addInstanceAtOffset(TS, View, 8, 8);
  auto *Access = addInstanceAtOffset(TS, Object, 5, 8);
  ArrangeAccessesHierarchically().runOnTypeSystem(TS);
  BOOST_CHECK_EQUAL(countInstanceEdges(View, Access), 0);
  BOOST_CHECK_EQUAL(countInstanceEdges(Object, Access), 1);
  BOOST_CHECK(TS.verifyDAG());
}

BOOST_AUTO_TEST_CASE(ArrangePropagatesMissingFunctionField) {
  LayoutTypeSystem TS;
  auto *Object = createRoot(TS, 32);
  auto *View = addInstanceAtOffset(TS, Object, 0, 24);
  View->IsTypeRoot = true;
  addInstanceAtOffset(TS, View, 8, 8);
  addInstanceAtOffset(TS, View, 16, 8);
  auto *Access = addInstanceAtOffset(TS, Object, 0, 8);
  ArrangeAccessesHierarchically().runOnTypeSystem(TS);
  BOOST_CHECK_EQUAL(countInstanceEdges(View, Access), 1);
  BOOST_CHECK_EQUAL(countInstanceEdges(Object, Access), 0);
  BOOST_CHECK(TS.verifyDAG());
}


BOOST_AUTO_TEST_CASE(DeduplicateFieldsPreservesUnknownPointerEvidence) {
  for (bool Nested : { false, true }) {
    LayoutTypeSystem TS;
    auto *Object = createRoot(TS, 16);
    auto *ScalarView = addInstanceAtOffset(TS, Object, 0, 16);
    auto *PointerView = addInstanceAtOffset(TS, Object, 0, 16);
    auto *Scalar = ScalarView;
    auto *Pointer = PointerView;
    if (Nested) {
      Scalar = addInstanceAtOffset(TS, ScalarView, 8, 8);
      Pointer = addInstanceAtOffset(TS, PointerView, 8, 8);
    }
    Pointer->HasUnknownPointee = true;
    auto *OtherObject = createRoot(TS, 24);
    TS.addInstanceLink(OtherObject, Scalar, OffsetExpression{ 8 });
    unsigned ScalarID = Scalar->ID, PointerID = Pointer->ID;
    DeduplicateFields().runOnTypeSystem(TS);
    BOOST_REQUIRE(not TS.getEqClasses().haveSameEqClass(ScalarID, PointerID));
    BOOST_CHECK(not Scalar->HasUnknownPointee);
    BOOST_CHECK(Pointer->HasUnknownPointee);
    BOOST_CHECK(TS.verifyDAG());
  }
}


BOOST_AUTO_TEST_CASE(ConstantStoresDoNotUnifyIndependentStorage) {
  TupleTree<model::Binary> Model;
  Model->Architecture() = model::Architecture::x86_64;
  auto Generic = model::PrimitiveType::make(model::PrimitiveKind::Generic, 8);
  auto &&[Prototype, PrototypeType] = Model->makeRawFunctionDefinition();
  Prototype.Architecture() = Model->Architecture();
  Prototype.FinalStackOffset() = 8;
  Prototype.addArgument(model::Register::rdi_x86_64, Generic.copy());
  Prototype.addReturnValue(model::Register::rax_x86_64, Generic.copy());
  model::Segment Segment(MetaAddress::fromGeneric(Model->Architecture(), 0x1000));
  Segment.VirtualSize() = Segment.FileSize() = 0x100;
  Segment.IsExecutable() = true;
  Model->Segments().insert(std::move(Segment));
  llvm::LLVMContext Context;
  llvm::Module Module("constants", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Integer = llvm::Type::getInt64Ty(Context);
  auto *Type = llvm::FunctionType::get(Integer, { Integer }, false);
  llvm::SmallVector<llvm::Value *> Addresses;
  for (unsigned Index = 0; Index != 8; ++Index) {
    auto Entry = MetaAddress::fromPC(Model->Architecture(), 0x1000 + Index * 16);
    Model->Functions()[Entry].Prototype() = PrototypeType.copy();
    auto *Function = llvm::Function::Create(Type,
                                            llvm::GlobalValue::ExternalLinkage,
                                            "store_" + std::to_string(Index),
                                            Module);
    FunctionTags::Isolated.addTo(Function);
    setMetaAddressMetadata(Function, FunctionEntryMDName, Entry);
    llvm::IRBuilder<> IR(llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Address = IR.CreateIntToPtr(Function->getArg(0), IR.getPtrTy());
    Addresses.push_back(Address);
    const uint64_t Values[] = { 0, 1, 8, UINT64_MAX };
    IR.CreateStore(IR.getInt64(Values[Index / 2]), Address);
    IR.CreateRet(Function->getArg(0));
  }
  BOOST_REQUIRE(Model->verify(true));
  BOOST_REQUIRE(not llvm::verifyModule(Module, &llvm::errs()));
  LayoutTypeSystem TS;
  DLATypeSystemLLVMBuilder Builder(TS, *Model);
  std::vector<llvm::Module *> Modules{ &Module };
  Builder.buildFromLLVMModules(Modules, { "constants" });
  llvm::SmallVector<unsigned> Accesses;
  for (auto *Address : Addresses) {
    auto &Values = Builder.getValues();
    auto It = llvm::find(Values, LayoutTypePtr(Address));
    BOOST_REQUIRE(It != Values.end());
    unsigned ID = std::distance(Values.begin(), It);
    auto Nodes = TS.getLayoutsRange();
    auto Node = llvm::find_if(Nodes, [ID](auto *N) { return N->ID == ID; });
    BOOST_REQUIRE(Node != Nodes.end());
    llvm::SmallVector<unsigned> Fields;
    for (const auto &Edge : (*Node)->Successors)
      if (isInstanceEdge(Edge) and Edge.first->Size == 8)
        Fields.push_back(Edge.first->ID);
    BOOST_REQUIRE_EQUAL(Fields.size(), 1);
    Accesses.push_back(Fields.front());
  }
  runStep<CollapseEqualitySCC>(TS);
  for (unsigned Index = 0; Index != Accesses.size(); Index += 2)
    BOOST_CHECK(not TS.getEqClasses().haveSameEqClass(Accesses[Index],
                                                     Accesses[Index + 1]));
}

BOOST_AUTO_TEST_CASE(DirectCallMapsDifferentRawPrototypesByRegister) {
  for (bool NarrowArgument : { false, true }) {
    TupleTree<model::Binary> Model;
    Model->Architecture() = model::Architecture::x86_64;
    auto Generic = model::PrimitiveType::make(model::PrimitiveKind::Generic, 8);
    auto &&[CallerPrototype, CallerType] = Model->makeRawFunctionDefinition();
    CallerPrototype.Architecture() = Model->Architecture();
    CallerPrototype.FinalStackOffset() = 8;
    CallerPrototype
      .addArgument(model::Register::rdx_x86_64,
                   model::PrimitiveType::make(model::PrimitiveKind::Generic,
                                              NarrowArgument ? 4 : 8));
    CallerPrototype.addArgument(model::Register::rdi_x86_64, Generic.copy());
    CallerPrototype.addReturnValue(model::Register::rax_x86_64, Generic.copy());
    auto &&[CalleePrototype, CalleeType] = Model->makeRawFunctionDefinition();
    CalleePrototype.Architecture() = Model->Architecture();
    CalleePrototype.FinalStackOffset() = 8;
    CalleePrototype.addArgument(model::Register::rcx_x86_64, Generic.copy());
    CalleePrototype.addArgument(model::Register::rdx_x86_64, Generic.copy());
    CalleePrototype.addArgument(model::Register::rdi_x86_64, Generic.copy());
    CalleePrototype.addReturnValue(model::Register::rax_x86_64, Generic.copy());
    model::Segment Segment(MetaAddress::fromGeneric(Model->Architecture(),
                                                    0x1000));
    Segment.VirtualSize() = 0x100;
    Segment.FileSize() = 0x100;
    Segment.IsExecutable() = true;
    Model->Segments().insert(std::move(Segment));
    llvm::LLVMContext Context;
    llvm::Module Module("register_mapping", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Integer = llvm::Type::getInt64Ty(Context);
    auto *FirstInteger = NarrowArgument ? llvm::Type::getInt32Ty(Context) :
                                          Integer;
    auto *CallerLLVMType = llvm::FunctionType::get(Integer,
                                                   { FirstInteger, Integer },
                                                   false);
    auto *CalleeLLVMType = llvm::FunctionType::get(Integer,
                                                   { Integer,
                                                     Integer,
                                                     Integer },
                                                   false);
    auto Declare = [&](llvm::FunctionType *Type,
                       llvm::StringRef Name,
                       uint64_t PC,
                       const model::UpcastableType &Prototype) {
      auto Entry = MetaAddress::fromPC(Model->Architecture(), PC);
      Model->Functions()[Entry].Prototype() = Prototype.copy();
      auto
        *Function = llvm::Function::Create(Type,
                                           llvm::GlobalValue::ExternalLinkage,
                                           Name,
                                           Module);
      FunctionTags::Isolated.addTo(Function);
      setMetaAddressMetadata(Function, FunctionEntryMDName, Entry);
      return Function;
    };
    auto *Caller = Declare(CallerLLVMType, "caller", 0x1000, CallerType);
    auto *Callee = Declare(CalleeLLVMType, "callee", 0x1010, CalleeType);
    llvm::IRBuilder<> IR(llvm::BasicBlock::Create(Context, "entry", Caller));
    auto *Call = IR.CreateCall(CallerLLVMType,
                               Callee,
                               { Caller->getArg(0), Caller->getArg(1) });
    setStringMetadata(Call,
                      PrototypeMDName,
                      Model->getTypeDefinitionReference(CallerPrototype.key())
                        .toString());
    IR.CreateRet(Call);
    BOOST_REQUIRE(Model->verify(true));
    BOOST_REQUIRE(not llvm::verifyModule(Module, &llvm::errs()));
    LayoutTypeSystem TS;
    DLATypeSystemLLVMBuilder Builder(TS, *Model);
    std::vector<llvm::Module *> Modules{ &Module };
    Builder.buildFromLLVMModules(Modules, { "register_mapping" });
    auto Node = [&](llvm::Value *Value) {
      auto &Values = Builder.getValues();
      auto It = llvm::find(Values, LayoutTypePtr(Value));
      revng_assert(It != Values.end());
      auto ID = static_cast<uint64_t>(std::distance(Values.begin(), It));
      for (auto *Node : TS.getLayoutsRange())
        if (Node->ID == ID)
          return Node;
      revng_abort();
    };
    BOOST_CHECK(countInstanceEdges(Node(Caller->getArg(0)),
                                   Node(Callee->getArg(1)))
                == (NarrowArgument ? 0 : 1));
    BOOST_CHECK(countInstanceEdges(Node(Caller->getArg(1)),
                                   Node(Callee->getArg(2)))
                == 1);
    BOOST_CHECK(countInstanceEdges(Node(Caller->getArg(0)),
                                   Node(Callee->getArg(0)))
                == 0);
    BOOST_CHECK(countInstanceEdges(Node(Caller->getArg(1)),
                                   Node(Callee->getArg(1)))
                == 0);
  }
}

BOOST_AUTO_TEST_CASE(SpilledValuesRetainOnlyProvenReachingStores) {
  for (unsigned Scenario = 0; Scenario != 12; ++Scenario) {
    TupleTree<model::Binary> Model;
    Model->Architecture() = model::Architecture::x86_64;
    auto Generic = model::PrimitiveType::make(model::PrimitiveKind::Generic, 8);
    auto &&[Prototype, PrototypeType] = Model->makeRawFunctionDefinition();
    Prototype.Architecture() = Model->Architecture();
    Prototype.FinalStackOffset() = 8;
    Prototype.addArgument(model::Register::rdi_x86_64, Generic.copy());
    Prototype.addArgument(model::Register::rsi_x86_64, Generic.copy());
    Prototype.addReturnValue(model::Register::rax_x86_64, Generic.copy());
    auto Entry = MetaAddress::fromPC(Model->Architecture(), 0x1000);
    Model->Functions()[Entry].Prototype() = PrototypeType.copy();
    model::Segment Segment(MetaAddress::fromGeneric(Model->Architecture(),
                                                    0x1000));
    Segment.VirtualSize() = Segment.FileSize() = 0x100;
    Segment.IsExecutable() = true;
    Model->Segments().insert(std::move(Segment));
    llvm::LLVMContext Context;
    llvm::Module Module("spilled-value", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Integer = llvm::Type::getInt64Ty(Context);
    auto *Type = llvm::FunctionType::get(Integer, { Integer, Integer }, false);
    auto *Function = llvm::Function::Create(Type,
                                            llvm::GlobalValue::ExternalLinkage,
                                            "spilled_value",
                                            Module);
    FunctionTags::Isolated.addTo(Function);
    setMetaAddressMetadata(Function, FunctionEntryMDName, Entry);
    auto *Start = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Read = llvm::BasicBlock::Create(Context, "read", Function);
    llvm::IRBuilder<> IR(Start);
    auto *Slot = IR.CreateAlloca(llvm::ArrayType::get(Integer, 4));
    IR.CreateStore(Function->getArg(0), Slot);
    if (Scenario == 6) {
      auto *Overwrite = llvm::BasicBlock::Create(Context,
                                                 "overwrite",
                                                 Function);
      IR.CreateCondBr(IR.CreateICmpEQ(Function->getArg(1), IR.getInt64(0)),
                      Overwrite,
                      Read);
      IR.SetInsertPoint(Overwrite);
      IR.CreateStore(Function->getArg(1), Slot);
    }
    IR.CreateBr(Read);
    IR.SetInsertPoint(Read);
    if (Scenario == 1)
      IR.CreateStore(Function->getArg(1), Slot);
    if (Scenario == 2) {
      auto *ClobberType = llvm::FunctionType::get(IR.getVoidTy(),
                                                  { IR.getPtrTy() },
                                                  false);
      auto Clobber = Module.getOrInsertFunction("unknown_write", ClobberType);
      IR.CreateCall(Clobber, { Slot });
    }
    if (Scenario == 3)
      IR.CreateStore(IR.getInt32(0), Slot);
    if (Scenario >= 7) {
      auto *Address = IR.CreatePtrToInt(Slot, Integer);
      uint64_t Offset = Scenario == 7 ? 16 : Scenario == 8 ? 4 : 0;
      auto *Other = IR.CreateIntToPtr(IR.CreateAdd(Address,
                                                   IR.getInt64(Offset)),
                                      IR.getPtrTy());
      if (Scenario == 10)
        Other = IR.CreateIntToPtr(IR.CreateAdd(Address, Function->getArg(1)),
                                  IR.getPtrTy());
      auto *Write = IR.CreateStore(Function->getArg(1), Other);
      if (Scenario == 11)
        Write->setAtomic(llvm::AtomicOrdering::Release);
    }
    auto *Reload = IR.CreateLoad(Integer, Slot);
    if (Scenario == 4)
      Reload->setVolatile(true);
    if (Scenario == 5)
      Reload->setAtomic(llvm::AtomicOrdering::Acquire);
    auto *Field = IR.CreateAdd(Reload, IR.getInt64(16));
    auto *Pointer = IR.CreateIntToPtr(Field, IR.getPtrTy());
    auto *Value = IR.CreateLoad(Integer, Pointer);
    IR.CreateRet(Value);
    BOOST_REQUIRE(Model->verify(true));
    BOOST_REQUIRE(not llvm::verifyModule(Module, &llvm::errs()));
    LayoutTypeSystem TS;
    DLATypeSystemLLVMBuilder Builder(TS, *Model);
    std::vector<llvm::Module *> Modules{ &Module };
    Builder.buildFromLLVMModules(Modules, { "spilled-value" });
    auto ID = [&Builder](llvm::Value *Value) {
      auto &Values = Builder.getValues();
      auto It = llvm::find(Values, LayoutTypePtr(Value));
      BOOST_REQUIRE(It != Values.end());
      return unsigned(std::distance(Values.begin(), It));
    };
    unsigned ReloadID = ID(Reload);
    unsigned FirstID = ID(Function->getArg(0));
    unsigned SecondID = ID(Function->getArg(1));
    runStep<CollapseEqualitySCC>(TS);
    BOOST_TEST_CONTEXT("scenario " << Scenario) {
      BOOST_CHECK_EQUAL(TS.getEqClasses().haveSameEqClass(ReloadID, FirstID),
                        Scenario == 0 or Scenario == 7);
      BOOST_CHECK_EQUAL(TS.getEqClasses().haveSameEqClass(ReloadID, SecondID),
                        Scenario == 1 or Scenario == 9);
    }
  }
}
