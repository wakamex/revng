/// \file ConvergenceMonitor.cpp
/// Detection of worklist loops that do not converge.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>

#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Progress.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Support/ConvergenceMonitor.h"

using namespace llvm;

namespace {

enum class Policy {
  Fail,
  Warn,
  Off
};

} // namespace

static cl::opt<Policy> ConvergenceCheck("convergence-check",
                                        cl::desc("What to do when a worklist "
                                                 "loop is found not to "
                                                 "converge"),
                                        cl::values(clEnumValN(Policy::Fail,
                                                              "fail",
                                                              "report it in "
                                                              "the trace and "
                                                              "stop"),
                                                   clEnumValN(Policy::Warn,
                                                              "warn",
                                                              "report it in "
                                                              "the trace and "
                                                              "on stderr, and "
                                                              "go on"),
                                                   clEnumValN(Policy::Off,
                                                              "off",
                                                              "do not check")),
                                        cl::init(Policy::Fail));

static cl::opt<unsigned> ConvergenceRepeats("convergence-repeats",
                                            cl::desc("Visits of one item that "
                                                     "report a change but "
                                                     "return it to an earlier "
                                                     "state before a worklist "
                                                     "loop is reported"),
                                            cl::init(3));

static cl::opt<uint64_t> ConvergenceVisitBound("convergence-visit-bound",
                                               cl::desc("Visits of one item "
                                                        "after which a "
                                                        "worklist loop is "
                                                        "reported"),
                                               cl::init(1000));

static cl::opt<uint64_t> MFPVisitBound("mfp-visit-bound",
                                       cl::desc("Visits of one label after "
                                                "which a monotone framework "
                                                "solve is reported"),
                                       cl::init(1000000));

/// Trace event names are written into JSON without escaping.
static std::string sanitize(StringRef Text) {
  std::string Result;
  Result.reserve(Text.size());
  for (char C : Text) {
    if (C == '"' or C == '\\' or static_cast<unsigned char>(C) < 0x20)
      Result.push_back('\'');
    else
      Result.push_back(C);
  }
  return Result;
}

namespace revng {

std::string NonConvergence::describe() const {
  std::string Result;
  raw_string_ostream Stream(Result);
  Stream << Loop << ": " << Reason << " after " << Visits << " visits";
  // The items that repeated most, then the most visited, by name for ties
  std::vector<const Item *> Sorted;
  for (const Item &I : Items)
    Sorted.push_back(&I);
  std::stable_sort(Sorted.begin(),
                   Sorted.end(),
                   [](const Item *A, const Item *B) {
                     return std::tie(B->Repeats, B->Visits)
                            < std::tie(A->Repeats, A->Visits);
                   });
  const size_t Shown = 16;
  for (size_t I = 0; I < Sorted.size() and I < Shown; ++I) {
    const Item &Entry = *Sorted[I];
    Stream << (I == 0 ? ": " : "; ") << Entry.Name << " visits "
           << Entry.Visits;
    if (Entry.Repeats != 0) {
      Stream << " repeats " << Entry.Repeats << " state "
             << format_hex(Entry.State, 18) << " first at visit "
             << Entry.FirstVisit << " again at visit " << Entry.RepeatVisit;
    }
  }
  if (Sorted.size() > Shown)
    Stream << "; and " << (Sorted.size() - Shown) << " more items";
  return sanitize(Result);
}

void emitTraceEvent(StringRef Name) {
  Task Event({}, sanitize(Name));
}

static void defaultHandler(const NonConvergence &Failure) {
  std::string Description = Failure.describe();
  bool Fail = ConvergenceCheck == Policy::Fail;
  emitTraceEvent((Twine(Fail ? "Convergence failure: " :
                               "Convergence warning: ")
                  + Description)
                   .str());
  errs() << "[convergence] " << (Fail ? "error: " : "warning: ") << Description
         << "\n";
  if (Fail) {
    report_fatal_error(Twine("a worklist loop does not converge: ")
                         + Description
                         + "\nRerun with -convergence-check=warn to continue "
                           "anyway, or with -convergence-check=off to skip "
                           "the check.",
                       false);
  }
}

static NonConvergenceHandler &handler() {
  static NonConvergenceHandler Handler = defaultHandler;
  return Handler;
}

NonConvergenceHandler setNonConvergenceHandler(NonConvergenceHandler New) {
  NonConvergenceHandler Old = std::move(handler());
  handler() = New ? std::move(New) : NonConvergenceHandler(defaultHandler);
  return Old;
}

void reportNonConvergence(const NonConvergence &Failure) {
  handler()(Failure);
}

bool convergenceChecksEnabled() {
  return ConvergenceCheck != Policy::Off;
}

uint64_t mfpVisitBound() {
  return convergenceChecksEnabled() ? MFPVisitBound.getValue() : 0;
}

ConvergenceMonitor::ConvergenceMonitor(StringRef Loop) :
  Loop(Loop.str()), Enabled(convergenceChecksEnabled()) {
}

ConvergenceMonitor::~ConvergenceMonitor() {
  if (Enabled and Visits != 0)
    emitTraceEvent(progress());
}

std::string ConvergenceMonitor::progress() const {
  return (Twine("Convergence progress: ") + Loop + ": visits "
          + Twine(Visits) + ", items " + Twine(Items.size()) + ", changed "
          + Twine(ChangedVisits) + ", changed items " + Twine(ChangedItems)
          + ", repeats " + Twine(Repeats) + ", max visits "
          + Twine(MaxVisits) + " " + MaxItem)
    .str();
}

NonConvergence ConvergenceMonitor::failure(StringRef Reason) const {
  NonConvergence Result;
  Result.Loop = Loop;
  Result.Reason = Reason.str();
  Result.Visits = Visits;
  for (const auto &Entry : Items) {
    const ItemState &State = Entry.getValue();
    if (State.Repeats == 0 and State.Visits != MaxVisits)
      continue;
    Result.Items.push_back({ .Name = Entry.getKey().str(),
                             .Visits = State.Visits,
                             .Repeats = State.Repeats,
                             .State = State.RepeatState,
                             .FirstVisit = State.RepeatFirstVisit,
                             .RepeatVisit = State.RepeatVisit });
  }
  // StringMap iterates in hash order
  llvm::sort(Result.Items,
             [](const NonConvergence::Item &A, const NonConvergence::Item &B) {
               return A.Name < B.Name;
             });
  return Result;
}

void ConvergenceMonitor::visit(StringRef Name, uint64_t State, bool Changed) {
  if (not Enabled)
    return;

  // DenseMap reserves the two largest keys
  State = std::min(State, ~uint64_t(0) - 2);

  ++Visits;
  ItemState &Item = Items[Name];
  ++Item.Visits;
  if (Item.Visits > MaxVisits) {
    MaxVisits = Item.Visits;
    MaxItem = Name.str();
  }

  auto [It, New] = Item.States.try_emplace(State, Visits);
  if (Changed) {
    ++ChangedVisits;
    if (not Item.EverChanged) {
      Item.EverChanged = true;
      ++ChangedItems;
    }

    if (not New) {
      // A change that returns the item to a state it already had
      ++Repeats;
      ++Item.Repeats;
      Item.RepeatState = State;
      Item.RepeatFirstVisit = It->second;
      Item.RepeatVisit = Visits;
    }
  }

  // Per visit, the events would add a few percent to a whole-image run
  auto Now = std::chrono::steady_clock::now();
  if ((Changed and not New) or Now - LastProgress >= std::chrono::seconds(1)) {
    LastProgress = Now;
    emitTraceEvent(progress());
  }

  if (Reported)
    return;

  if (Item.Repeats >= ConvergenceRepeats) {
    Reported = true;
    reportNonConvergence(failure("oscillation: items report changes but "
                                 "return to states they already had"));
  } else if (Item.Visits > ConvergenceVisitBound) {
    Reported = true;
    reportNonConvergence(failure((Twine("visit bound: ") + Name
                                  + " visited more than "
                                  + Twine(ConvergenceVisitBound.getValue())
                                  + " times")
                                   .str()));
  }
}

} // namespace revng
