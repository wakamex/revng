//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE SpeculativeWorklist
bool init_unit_test();
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#include "boost/test/unit_test.hpp"

#include "revng/ADT/Queue.h"
#include "revng/ADT/SpeculativeWorklist.h"

// A fixpoint over a call graph, shaped like detect-abi's preliminary analysis:
// analyzing a function reads the summaries of its callees, a changed summary
// enqueues its callers, and the callers of an inline function's callers too.
// Functions without callees need no work from a worker.
struct Program {
  std::vector<std::vector<unsigned>> Callees;
  std::vector<std::vector<unsigned>> Callers;
  std::vector<bool> Inline;

  Program(unsigned Size, std::mt19937 &Random) :
    Callees(Size), Callers(Size), Inline(Size) {
    for (unsigned F = 0; F < Size; ++F) {
      Inline[F] = Random() % 8 == 0;
      unsigned Count = Random() % 4;
      for (unsigned I = 0; I < Count; ++I) {
        unsigned Callee = Random() % Size;
        if (std::find(Callees[F].begin(), Callees[F].end(), Callee)
            == Callees[F].end()) {
          Callees[F].push_back(Callee);
          Callers[Callee].push_back(F);
        }
      }
    }
  }

  using Reads = std::map<unsigned, unsigned>;

  static unsigned analyze(unsigned F, const Reads &Read) {
    unsigned Result = F % 3;
    for (auto [Callee, Summary] : Read)
      Result = std::max(Result, std::min(Summary + 1 + Callee % 2, 12U));
    return Result;
  }
};

using Commit = std::tuple<unsigned, unsigned, bool>;

struct Run {
  const Program &P;
  std::vector<unsigned> Summaries;
  std::vector<Commit> Log;

  explicit Run(const Program &P) : P(P), Summaries(P.Callees.size(), 0) {}

  Program::Reads read(unsigned F) const {
    Program::Reads Result;
    for (unsigned Callee : P.Callees[F])
      Result[Callee] = Summaries[Callee];
    return Result;
  }

  /// Commit the result of F, calling Insert on the functions to analyze again
  template<typename InsertT>
  void commit(unsigned F, unsigned Result, InsertT Insert) {
    bool Changed = Result != Summaries[F];
    Summaries[F] = Result;
    Log.emplace_back(F, Result, Changed);
    if (not Changed)
      return;

    OnceQueue<unsigned> InlineWorklist;
    InlineWorklist.insert(F);
    while (not InlineWorklist.empty()) {
      unsigned Node = InlineWorklist.pop();
      for (unsigned Caller : P.Callers[Node]) {
        if (P.Inline[Caller])
          InlineWorklist.insert(Caller);
        Insert(Caller);
      }
    }
  }
};

static std::vector<Commit> sequential(const Program &P) {
  Run R(P);
  UniquedQueue<unsigned> Queue;
  for (unsigned F = 0; F < P.Callees.size(); ++F)
    Queue.insert(F);

  while (not Queue.empty()) {
    unsigned F = Queue.pop();
    R.commit(F, Program::analyze(F, R.read(F)), [&](unsigned Caller) {
      Queue.insert(Caller);
    });
  }

  return R.Log;
}

struct Ticket {
  unsigned Function;
  Program::Reads Read;
  uint64_t Delay;
};

static std::vector<Commit> speculative(const Program &P,
                                       unsigned Workers,
                                       size_t Window,
                                       size_t Budget,
                                       uint32_t Seed,
                                       bool UseDependencies,
                                       size_t &Repeated) {
  Run R(P);
  std::mt19937 Random(Seed);
  SpeculativeWorklist<unsigned, Ticket, unsigned> Worklist;
  for (unsigned F = 0; F < P.Callees.size(); ++F)
    Worklist.insert(F);

  using Callbacks = SpeculativeWorklist<unsigned, Ticket, unsigned>::Callbacks;
  Callbacks C;
  C.Prepare = [&](unsigned F) -> std::optional<Ticket> {
    if (P.Callees[F].empty())
      return std::nullopt;
    return Ticket{ F, R.read(F), Random() % 50 };
  };
  C.Cost = [](const Ticket &T) { return T.Read.size(); };
  C.Work = [](const Ticket &T) {
    std::this_thread::sleep_for(std::chrono::microseconds(T.Delay));
    return Program::analyze(T.Function, T.Read);
  };
  C.Commit = [&](unsigned F,
                 std::optional<Ticket> &T,
                 std::optional<unsigned> &Result) {
    unsigned Final = 0;
    if (T.has_value()) {
      if (T->Read != R.read(F))
        return false;
      Final = *Result;
    } else {
      Final = Program::analyze(F, R.read(F));
    }

    R.commit(F, Final, [&](unsigned Caller) { Worklist.insert(Caller); });
    return true;
  };
  C.Reads = [&](unsigned F) {
    if (not UseDependencies)
      return std::vector<unsigned>();
    return P.Callees[F];
  };

  Worklist.run(C, Workers, Window, Budget);
  Repeated += Worklist.statistics().Repeated;
  return R.Log;
}

BOOST_AUTO_TEST_CASE(CommitsAreSequential) {
  // Without dependencies, items are prepared ahead of the callees they read,
  // so commits have to reject stale preparations
  size_t Repeated[2] = { 0, 0 };
  for (uint32_t Seed = 0; Seed < 100; ++Seed) {
    std::mt19937 Random(Seed);
    Program P(40 + Seed % 40, Random);
    std::vector<Commit> Expected = sequential(P);

    for (bool UseDependencies : { true, false }) {
      for (auto [Workers, Window, Budget] :
           { std::tuple<unsigned, size_t, size_t>{ 1, 1, 1 },
             { 1, 8, 100 },
             { 2, 4, 2 },
             { 4, 16, 100 },
             { 8, 32, 6 } }) {
        auto Actual = speculative(P,
                                  Workers,
                                  Window,
                                  Budget,
                                  Seed,
                                  UseDependencies,
                                  Repeated[UseDependencies]);
        BOOST_TEST_CONTEXT("seed " << Seed << ", " << Workers << " workers, "
                                   << "window " << Window << ", budget "
                                   << Budget << ", dependencies "
                                   << UseDependencies) {
          BOOST_TEST(Actual.size() == Expected.size());
          BOOST_TEST((Actual == Expected));
        }
      }
    }
  }

  BOOST_TEST(Repeated[false] > 0);
}
