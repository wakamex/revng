//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE ConvergenceMonitor
bool init_unit_test();
#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <sys/wait.h>
#include <unistd.h>
}

#include "boost/test/unit_test.hpp"

#include "llvm/ADT/Hashing.h"

#include "revng/Support/ConvergenceMonitor.h"

// A small model of DetectABI's preliminary analysis, with the defect that
// made it cycle: summaries are sets of registers ordered by name, while the
// test for "the new summary is contained in the recorded one" runs
// std::includes with the default pointer comparison.

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
Register Registers[] = { { "e" }, { "d" }, { "c" }, { "b" }, { "a" } };
const Register *E = &Registers[0];
const Register *D = &Registers[1];
const Register *C = &Registers[2];
const Register *B = &Registers[3];
const Register *A = &Registers[4];

/// Registers that summaries of known callees pass on to their callers
const RegisterSet Scratch{ A, C, E };
/// What an unknown callee clobbers
const RegisterSet Unknown{ B, D };

bool includesByAddress(const RegisterSet &Outer, const RegisterSet &Inner) {
  return std::includes(Outer.begin(), Outer.end(), Inner.begin(), Inner.end());
}

bool includesByName(const RegisterSet &Outer, const RegisterSet &Inner) {
  return std::includes(Outer.begin(),
                       Outer.end(),
                       Inner.begin(),
                       Inner.end(),
                       ByName());
}

using Includes = std::function<bool(const RegisterSet &, const RegisterSet &)>;

struct Function {
  std::string Name;
  RegisterSet Own;
  std::vector<int> Callees;
};

uint64_t hashOf(const RegisterSet &Set) {
  llvm::hash_code Hash = llvm::hash_combine(Set.size());
  for (const Register *R : Set)
    Hash = llvm::hash_combine(Hash, llvm::StringRef(R->Name));
  return Hash;
}

/// Run the worklist with \p IncludedIn as the containment test, for at most
/// \p Limit visits. \return the number of visits.
size_t runPreliminaryAnalysis(const Includes &IncludedIn, size_t Limit) {
  // Each function calls itself and the next one: a recursive cycle of three
  std::vector<Function> Functions{ { "f1", { A }, { 0, 1 } },
                                   { "f2", { C }, { 1, 2 } },
                                   { "f3", { E }, { 2, 0 } } };
  std::map<int, RegisterSet> Recorded;
  std::deque<int> Queue{ 2, 1, 0 };
  revng::ConvergenceMonitor Monitor("test preliminary analysis");

  size_t Visits = 0;
  while (not Queue.empty() and Visits < Limit) {
    int Index = Queue.front();
    Queue.pop_front();
    ++Visits;

    // A function clobbers its own registers, the scratch registers its known
    // callees clobber, and everything an unknown callee may clobber
    RegisterSet New = Functions[Index].Own;
    for (int Callee : Functions[Index].Callees) {
      auto It = Recorded.find(Callee);
      if (It == Recorded.end()) {
        New.insert(Unknown.begin(), Unknown.end());
      } else {
        for (const Register *R : It->second)
          if (Scratch.contains(R))
            New.insert(R);
      }
    }

    // As FunctionSummaryOracle::registerLocalFunction: changed unless the new
    // summary is contained in the recorded one, then record the union
    auto [It, First] = Recorded.try_emplace(Index, New);
    bool Changed = First or not IncludedIn(It->second, New);
    It->second.insert(New.begin(), New.end());
    Monitor.visit(Functions[Index].Name, hashOf(It->second), Changed);

    if (Changed) {
      for (int Caller = 0; Caller < 3; ++Caller) {
        const auto &Callees = Functions[Caller].Callees;
        bool Calls = std::find(Callees.begin(), Callees.end(), Index)
                     != Callees.end();
        if (Calls
            and std::find(Queue.begin(), Queue.end(), Caller) == Queue.end())
          Queue.push_back(Caller);
      }
    }
  }
  return Visits;
}

/// For the duration of a test, record reports instead of failing; the loops
/// under test bound their own visits.
struct RecordingHandler {
  revng::NonConvergenceHandler Previous;
  std::vector<revng::NonConvergence> Reports;

  RecordingHandler() {
    Previous = revng::setNonConvergenceHandler(
      [this](const revng::NonConvergence &Failure) {
        Reports.push_back(Failure);
      });
  }

  ~RecordingHandler() { revng::setNonConvergenceHandler(Previous); }
};

/// Run \p Body in a child process with the default handler, which fails the
/// process; \return its wait status and standard error.
std::pair<int, std::string> inChild(const std::function<void()> &Body) {
  int Pipe[2];
  BOOST_REQUIRE(pipe(Pipe) == 0);
  pid_t Child = fork();
  BOOST_REQUIRE(Child >= 0);
  if (Child == 0) {
    dup2(Pipe[1], 2);
    close(Pipe[0]);
    // A loop the check misses would never end
    alarm(60);
    Body();
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
  return { Status, Output };
}

} // namespace

BOOST_AUTO_TEST_CASE(RegistersSortOppositeToAddresses) {
  // The premise of the defect: a subset whose missing register sorts between
  // two present ones is reported as not included when compared by address
  RegisterSet Outer{ A, B, C, D, E };
  RegisterSet Inner{ A, C, E };
  BOOST_TEST(includesByName(Outer, Inner));
  BOOST_TEST(not includesByAddress(Outer, Inner));
}

BOOST_AUTO_TEST_CASE(ConsistentComparisonConverges) {
  RecordingHandler Handler;
  size_t Visits = runPreliminaryAnalysis(includesByName, 1000);
  BOOST_TEST(Visits < 20u);
  BOOST_TEST(Handler.Reports.empty());
}

BOOST_AUTO_TEST_CASE(InconsistentComparisonIsReported) {
  RecordingHandler Handler;
  // Without the check the loop would never end: it stops at the limit
  size_t Visits = runPreliminaryAnalysis(includesByAddress, 1000);
  BOOST_TEST(Visits == 1000u);
  BOOST_REQUIRE(Handler.Reports.size() == 1u);
  const revng::NonConvergence &Failure = Handler.Reports.front();
  BOOST_TEST(Failure.Loop == "test preliminary analysis");
  BOOST_TEST(Failure.Reason.starts_with("oscillation"));
  // Every function of the cycle returned to a state it already had, and the
  // loop was reported within a few rounds of the cycle starting
  BOOST_TEST(Failure.Items.size() == 3u);
  for (const auto &Item : Failure.Items) {
    BOOST_TEST(Item.Repeats >= 1u);
    BOOST_TEST(Item.FirstVisit < Item.RepeatVisit);
  }
  BOOST_TEST(Failure.Visits < 30u);
  std::string Description = Failure.describe();
  BOOST_TEST(Description.find("f1 visits") != std::string::npos);
  BOOST_TEST(Description.find('"') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(InconsistentComparisonFailsByDefault) {
  // The default handler stops the process with the diagnostic
  auto [Status, Output] = inChild([] {
    runPreliminaryAnalysis(includesByAddress, 1000000000);
  });
  BOOST_TEST(WIFEXITED(Status));
  BOOST_TEST(WEXITSTATUS(Status) == 1);
  BOOST_TEST(Output.find("[convergence] error: test preliminary analysis: "
                         "oscillation")
             != std::string::npos);
  BOOST_TEST(Output.find("does not converge") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(UnchangedVisitsMayRepeatStates) {
  // A visit that reports no change re-enqueues nothing, so returning to an
  // earlier state is not an oscillation
  RecordingHandler Handler;
  revng::ConvergenceMonitor Monitor("test unchanged");
  for (int I = 0; I < 100; ++I)
    Monitor.visit("item", I % 2, I < 2);
  BOOST_TEST(Monitor.repeats() == 0u);
  BOOST_TEST(Handler.Reports.empty());
}

BOOST_AUTO_TEST_CASE(GrowingStatesHitTheVisitBound) {
  // An item that keeps changing to new states is reported once it exceeds the
  // visit bound (1000 by default)
  RecordingHandler Handler;
  revng::ConvergenceMonitor Monitor("test growth");
  for (uint64_t I = 0; I < 2000; ++I)
    Monitor.visit("item", I, true);
  BOOST_REQUIRE(Handler.Reports.size() == 1u);
  BOOST_TEST(Handler.Reports.front().Reason.starts_with("visit bound"));
  BOOST_TEST(Handler.Reports.front().Visits == 1001u);
}
