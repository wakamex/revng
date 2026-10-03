//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE MFPExtraState
bool init_unit_test();
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <sys/wait.h>
#include <unistd.h>
}

#include "boost/test/unit_test.hpp"

#include "llvm/ADT/GraphTraits.h"

#include "revng/ADT/GenericGraph.h"
#include "revng/MFP/MFP.h"
#include "revng/MFP/SetLattices.h"
#include "revng/Support/ConvergenceMonitor.h"

// Each node has a name and an ordered list of integer "operations" that the
// transfer function adds to a `std::set<int>` lattice. The `ExtraState`
// recording surface uses `(NodeName, OperationIndex)` as its key, so each
// individual operation can be observed before/after.
struct OperationNodeData {
  std::string Name;
  std::vector<int> Operations;
};

using OperationNode = ForwardNode<OperationNodeData>;
using OperationGraph = GenericGraph<OperationNode>;

using OperationKey = std::pair<std::string, size_t>;
using IntSet = std::set<int>;

struct OperationsMFI : public SetUnionLattice<IntSet> {
  using Label = OperationNode *;
  using GraphType = OperationGraph *;
  using LatticeElement = IntSet;
  using ExtraStateKey = OperationKey;
  using ExtraStateType = mfp::ExtraState<OperationKey, LatticeElement>;

  LatticeElement applyTransferFunction(Label Node,
                                       const LatticeElement &In,
                                       ExtraStateType &State) const {
    LatticeElement Current = In;
    for (size_t I = 0; I < Node->Operations.size(); ++I) {
      OperationKey K{ Node->Name, I };
      State.registerBefore(K, Current);
      Current.insert(Node->Operations[I]);
      State.registerAfter(K, Current);
    }
    return Current;
  }
};

static_assert(mfp::MonotoneFrameworkInstance<OperationsMFI>);

namespace {

struct DiamondGraph {
  // A diamond:
  //
  //         Entry:[1]
  //         /        \
  //   Left:[2]      Right:[3]
  //         \        /
  //           Tail:[4]
  //
  OperationGraph Graph;
  OperationNode *Entry = nullptr;
  OperationNode *Left = nullptr;
  OperationNode *Right = nullptr;
  OperationNode *Tail = nullptr;

  DiamondGraph() {
    Entry = Graph.addNode(OperationNodeData{ "Entry", { 1 } });
    Left = Graph.addNode(OperationNodeData{ "Left", { 2 } });
    Right = Graph.addNode(OperationNodeData{ "Right", { 3 } });
    Tail = Graph.addNode(OperationNodeData{ "Tail", { 4 } });
    Graph.setEntryNode(Entry);
    Entry->addSuccessor(Left);
    Entry->addSuccessor(Right);
    Left->addSuccessor(Tail);
    Right->addSuccessor(Tail);
  }
};

} // namespace

// The fixed point of a simple set-union analysis on a diamond graph is the
// union of every reachable predecessor's contributions. After convergence
// every node downstream of `Entry` must observe `1` on entry; `Tail` must see
// the contributions of both `Left` and `Right`.
BOOST_AUTO_TEST_CASE(DiamondMFP) {
  DiamondGraph G;

  OperationsMFI MFI;
  IntSet Bottom;
  IntSet ExtremalValue;
  std::vector<OperationNode *> ExtremalLabels{ G.Entry };

  mfp::MFPConfiguration<OperationsMFI> Configuration{
    .Instance = &MFI,
    .Flow = &G.Graph,
    .Bottom = &Bottom,
    .ExtremalValue = &ExtremalValue,
    .ExtremalLabels = &ExtremalLabels,
  };

  auto Result = mfp::getMaximalFixedPoint<OperationsMFI>(Configuration);

  BOOST_TEST(Result.at(G.Entry).InValue == IntSet{});
  BOOST_TEST(Result.at(G.Entry).OutValue == IntSet({ 1 }));

  BOOST_TEST(Result.at(G.Left).InValue == IntSet({ 1 }));
  BOOST_TEST(Result.at(G.Left).OutValue == IntSet({ 1, 2 }));

  BOOST_TEST(Result.at(G.Right).InValue == IntSet({ 1 }));
  BOOST_TEST(Result.at(G.Right).OutValue == IntSet({ 1, 3 }));

  // `Tail` joins both branches, so its incoming value must be `{1, 2, 3}` and
  // its outgoing value must additionally contain its own contribution `4`.
  BOOST_TEST(Result.at(G.Tail).InValue == IntSet({ 1, 2, 3 }));
  BOOST_TEST(Result.at(G.Tail).OutValue == IntSet({ 1, 2, 3, 4 }));
}

// Verify the analysis reaches the same fixed point on a graph with a back
// edge (and a multi-step node), exercising the worklist.
BOOST_AUTO_TEST_CASE(LoopMFP) {
  // A:[1] --> B:[2,3] --> C:[4]
  //              ^         /
  //              \________/
  OperationGraph Graph;
  auto *A = Graph.addNode(OperationNodeData{ "A", { 1 } });
  auto *B = Graph.addNode(OperationNodeData{ "B", { 2, 3 } });
  auto *C = Graph.addNode(OperationNodeData{ "C", { 4 } });
  Graph.setEntryNode(A);
  A->addSuccessor(B);
  B->addSuccessor(C);
  C->addSuccessor(B);

  OperationsMFI MFI;
  IntSet Bottom;
  IntSet ExtremalValue;
  std::vector<OperationNode *> ExtremalLabels{ A };

  mfp::MFPConfiguration<OperationsMFI> Configuration{
    .Instance = &MFI,
    .Flow = &Graph,
    .Bottom = &Bottom,
    .ExtremalValue = &ExtremalValue,
    .ExtremalLabels = &ExtremalLabels,
  };

  auto Result = mfp::getMaximalFixedPoint<OperationsMFI>(Configuration);

  // The loop forces B and C's incoming values to include {1, 2, 3, 4} once
  // the analysis converges.
  BOOST_TEST(Result.at(A).OutValue == IntSet({ 1 }));
  BOOST_TEST(Result.at(B).InValue == IntSet({ 1, 2, 3, 4 }));
  BOOST_TEST(Result.at(B).OutValue == IntSet({ 1, 2, 3, 4 }));
  BOOST_TEST(Result.at(C).InValue == IntSet({ 1, 2, 3, 4 }));
  BOOST_TEST(Result.at(C).OutValue == IntSet({ 1, 2, 3, 4 }));
}

// Same diamond as the first case, but the caller hands in an `ExtraState`
// pre-populated with a few interesting `(operation, position)` pairs and
// confirms the recorded values match the per-operation lattice values at the
// fixed point. The ExtraState observation is a check on top of the regular
// MFP result, not the focus of the test.
BOOST_AUTO_TEST_CASE(ExtraStateRecordingOnDiamond) {
  DiamondGraph G;

  using State = mfp::ExtraState<OperationKey, IntSet>;
  State S;

  // Mark a single point per node.
  OperationKey EntryOp0{ "Entry", 0 };
  OperationKey LeftOp0{ "Left", 0 };
  OperationKey TailOp0{ "Tail", 0 };
  S.registerAsInterestingBefore(EntryOp0);
  S.registerAsInterestingAfter(LeftOp0);
  S.registerAsInterestingBefore(TailOp0);

  OperationsMFI MFI;
  IntSet Bottom;
  IntSet ExtremalValue;
  std::vector<OperationNode *> ExtremalLabels{ G.Entry };

  mfp::MFPConfiguration<OperationsMFI> Configuration{
    .Instance = &MFI,
    .Flow = &G.Graph,
    .Bottom = &Bottom,
    .ExtremalValue = &ExtremalValue,
    .ExtremalLabels = &ExtremalLabels,
    .ExtraState = &S
  };

  auto Result = mfp::getMaximalFixedPoint<OperationsMFI>(Configuration);

  // Sanity: the fixed-point lattice values must match the diamond test above.
  BOOST_TEST(Result.at(G.Tail).OutValue == IntSet({ 1, 2, 3, 4 }));

  // The recorded values agree with the labels' InValue / partial OutValue.
  BOOST_TEST(S.getBefore(EntryOp0) == IntSet{});
  BOOST_TEST(S.getAfter(LeftOp0) == IntSet({ 1, 2 }));
  BOOST_TEST(S.getBefore(TailOp0) == IntSet({ 1, 2, 3 }));
}

// A gen/kill analysis over registers whose sets are ordered by name, while the
// buggy comparison runs std::includes by address, as
// FunctionSummary::containedOrEqual did. A strict subset whose missing
// register sorts between two present ones then compares as not included, so
// the solver re-enqueues a successor whose value the combination did not
// change, forever. The solver must report that instead of looping.

namespace {

struct Register {
  const char *Name;
};

struct ByName {
  bool operator()(const Register *A, const Register *B) const {
    return std::strcmp(A->Name, B->Name) < 0;
  }
};

using RegisterSet = std::set<const Register *, ByName>;

// Array order is address order, so the names sort opposite to the addresses
Register Registers[] = { { "x" }, { "n" }, { "m" }, { "b" }, { "a" } };
const Register *X = &Registers[0];
const Register *N = &Registers[1];
const Register *M = &Registers[2];
const Register *B = &Registers[3];
const Register *A = &Registers[4];

struct GenKill {
  RegisterSet Gen;
  RegisterSet Kill;
};

using GenKillNode = ForwardNode<GenKill>;
using GenKillGraph = GenericGraph<GenKillNode>;

struct GenKillMFI {
  using Label = GenKillNode *;
  using GraphType = GenKillGraph *;
  using LatticeElement = RegisterSet;

  bool CompareByAddress = false;

  LatticeElement combineValues(const LatticeElement &Left,
                               const LatticeElement &Right) const {
    LatticeElement Result = Left;
    Result.insert(Right.begin(), Right.end());
    return Result;
  }

  bool isLessOrEqual(const LatticeElement &Left,
                     const LatticeElement &Right) const {
    if (CompareByAddress)
      return std::includes(Right.begin(), Right.end(), Left.begin(), Left.end());
    return std::includes(Right.begin(),
                         Right.end(),
                         Left.begin(),
                         Left.end(),
                         ByName());
  }

  LatticeElement applyTransferFunction(Label Node,
                                       const LatticeElement &In,
                                       mfp::NoExtraState &) const {
    LatticeElement Out;
    for (const Register *R : In)
      if (not Node->Kill.contains(R))
        Out.insert(R);
    Out.insert(Node->Gen.begin(), Node->Gen.end());
    return Out;
  }
};

static_assert(mfp::MonotoneFrameworkInstance<GenKillMFI>);

/// Solve on Entry -> {L, R}, L <-> R, where each side kills a register the
/// other's incoming value keeps between two others.
mfp::MFIResultMap<GenKillMFI> solveGenKill(bool CompareByAddress) {
  GenKillGraph Graph;
  auto *Entry = Graph.addNode(GenKill{ { M, N, X }, {} });
  auto *L = Graph.addNode(GenKill{ { A }, { B, M } });
  auto *R = Graph.addNode(GenKill{ { B }, { A, N } });
  Graph.setEntryNode(Entry);
  Entry->addSuccessor(L);
  Entry->addSuccessor(R);
  L->addSuccessor(R);
  R->addSuccessor(L);

  GenKillMFI MFI{ .CompareByAddress = CompareByAddress };
  RegisterSet Bottom;
  RegisterSet ExtremalValue;
  std::vector<GenKillNode *> ExtremalLabels{ Entry };
  mfp::MFPConfiguration<GenKillMFI> Configuration{
    .Instance = &MFI,
    .Flow = &Graph,
    .Bottom = &Bottom,
    .ExtremalValue = &ExtremalValue,
    .ExtremalLabels = &ExtremalLabels,
  };
  return mfp::getMaximalFixedPoint<GenKillMFI>(Configuration);
}

} // namespace

BOOST_AUTO_TEST_CASE(ConsistentComparisonConverges) {
  std::vector<revng::NonConvergence> Reports;
  auto Previous = revng::setNonConvergenceHandler(
    [&Reports](const revng::NonConvergence &Failure) {
      Reports.push_back(Failure);
    });
  auto Result = solveGenKill(false);
  revng::setNonConvergenceHandler(Previous);
  BOOST_TEST(Result.size() == 3u);
  BOOST_TEST(Reports.empty());
}

BOOST_AUTO_TEST_CASE(InconsistentComparisonFailsTheSolve) {
  // Without the check the solve never ends, so it runs in a child process
  // with the default handler, which stops it with the diagnostic
  int Pipe[2];
  BOOST_REQUIRE(pipe(Pipe) == 0);
  pid_t Child = fork();
  BOOST_REQUIRE(Child >= 0);
  if (Child == 0) {
    dup2(Pipe[1], 2);
    close(Pipe[0]);
    alarm(60);
    solveGenKill(true);
    _exit(0);
  }
  close(Pipe[1]);
  std::string Output;
  char Buffer[4096];
  ssize_t Read = 0;
  while ((Read = read(Pipe[0], Buffer, sizeof(Buffer))) > 0)
    Output.append(Buffer, Read);
  close(Pipe[0]);
  int Status = 0;
  waitpid(Child, &Status, 0);

  BOOST_TEST(WIFEXITED(Status));
  BOOST_TEST(WEXITSTATUS(Status) == 1);
  BOOST_TEST(Output.find("[convergence] error: monotone framework")
             != std::string::npos);
  BOOST_TEST(Output.find("GenKillMFI") != std::string::npos);
  BOOST_TEST(Output.find("oscillation") != std::string::npos);
}
