//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE ControlFlowGraph
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "revng/EarlyFunctionAnalysis/ControlFlowGraph.h"
#include "revng/Model/Binary.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

using Edge = UpcastablePointer<efa::FunctionEdgeBase>;

// A call to a function the model says does not return leaves the code after
// it unreachable, as when the callee is known only after the CFG was
// collected, which is what calls through a slot to a panic function in Rust
// binaries do: their fallthrough is an exception landing pad
BOOST_AUTO_TEST_CASE(CallToNoReturnLeavesFallthroughUnreachable) {
  TupleTree<model::Binary> Binary;
  Binary->Architecture() = model::Architecture::x86_64;
  auto Address = [](uint64_t Value) {
    return MetaAddress::fromPC(model::Architecture::x86_64, Value);
  };
  MetaAddress Caller = Address(0x1000);
  MetaAddress Callee = Address(0x2000);
  Binary->Functions()[Caller];
  using namespace model::FunctionAttribute;
  Binary->Functions()[Callee].Attributes().insert(NoReturn);

  using namespace efa::FunctionEdgeType;
  efa::BasicBlock Call{ BasicBlockID(Caller) };
  Call.End() = Address(0x1005);
  Call.Successors().insert(Edge::make<efa::CallEdge>(BasicBlockID(Callee),
                                                     FunctionCall));
  efa::BasicBlock LandingPad{ BasicBlockID(Address(0x1005)) };
  LandingPad.End() = Address(0x1006);
  LandingPad.Successors().insert(Edge::make<efa::FunctionEdge>(BasicBlockID(),
                                                               Return));
  SortedVector<efa::BasicBlock> Blocks;
  Blocks.insert(Call);
  Blocks.insert(LandingPad);

  efa::ControlFlowGraph CFG(Caller, "", Blocks);
  BOOST_TEST(not CFG.verify(*Binary));

  CFG.removeUnreachableBlocks(*Binary);
  BOOST_TEST(CFG.Blocks().size() == 1U);
  BOOST_TEST(CFG.verify(*Binary));
}
