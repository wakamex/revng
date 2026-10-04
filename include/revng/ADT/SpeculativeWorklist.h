#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "revng/Support/Assert.h"

/// A worklist algorithm whose items are analyzed in parallel but take effect in
/// the order the sequential algorithm gives them.
///
/// The sequential algorithm pops the head of a FIFO queue without duplicates
/// (`UniquedQueue`), analyzes it and commits the result, and committing may
/// insert items. Here the main thread prepares items ahead of the head, worker
/// threads run the expensive part of the analysis, and the main thread commits
/// strictly in queue order.
///
/// An item stays in the queue until it commits. Inserting an item that is
/// being analyzed is therefore a no-op, as it is in the sequential algorithm,
/// where the item is still queued at that point, and the order of the queue is
/// the sequential one at every commit.
///
/// Preparing an item ahead of the head reads the state before the commits of
/// the items ahead of it. `Commit` must check that what the preparation read
/// still holds and return false otherwise: the item is then prepared again,
/// against the state as of its commit. If `Commit` accepts only results the
/// sequential algorithm would compute at that point, the commits, and so the
/// output, are the sequential ones for any number of workers and any order in
/// which the workers finish.
template<typename ItemT, typename TicketT, typename ResultT>
class SpeculativeWorklist {
public:
  struct Callbacks {
    /// Main thread: prepare \p Item for a worker, or return std::nullopt if
    /// its commit needs no work
    std::function<std::optional<TicketT>(ItemT)> Prepare;

    /// The cost of a ticket in flight, compared with the budget
    std::function<size_t(const TicketT &)> Cost;

    /// Worker thread: the expensive part of the analysis. It must not touch
    /// anything the main thread or another worker touches.
    std::function<ResultT(const TicketT &)> Work;

    /// Main thread, in queue order: validate and commit \p Item, inserting
    /// items with `insert`, or return false, without inserting anything, to
    /// have \p Item prepared again. \p Result is set whenever \p Ticket is.
    std::function<bool(ItemT Item,
                       std::optional<TicketT> &Ticket,
                       std::optional<ResultT> &Result)>
      Commit;

    /// Whether preparing \p Item reads what committing \p Earlier writes. An
    /// item is not prepared ahead of an earlier item it depends on, since its
    /// preparation would most likely have to be repeated.
    std::function<bool(ItemT Item, ItemT Earlier)> DependsOn;
  };

  struct Statistics {
    size_t Prepared = 0;
    size_t Repeated = 0;
    size_t Committed = 0;
  };

private:
  enum class State { Waiting, Queued, Running, Done };

  struct Entry {
    ItemT Item;
    State Status = State::Waiting;
    std::optional<TicketT> Ticket;
    std::optional<ResultT> Result;
    size_t Cost = 0;

    explicit Entry(ItemT Item) : Item(Item) {}
  };

private:
  std::deque<ItemT> Pending;
  std::set<ItemT> Members;
  Statistics Stats;

  // Shared with the workers
  std::mutex Lock;
  std::condition_variable WorkAvailable;
  std::condition_variable WorkDone;
  std::deque<Entry *> Jobs;
  size_t Completions = 0;
  bool Stopping = false;

public:
  /// Insert \p Item at the back unless it is already queued
  void insert(ItemT Item) {
    if (Members.insert(Item).second)
      Pending.push_back(Item);
  }

  size_t size() const { return Members.size(); }

  const Statistics &statistics() const { return Stats; }

  /// Run the worklist to completion with \p Workers threads, preparing at most
  /// \p Window items ahead of the head and admitting new work while the cost
  /// in flight is below \p Budget. The head is always admitted.
  void run(const Callbacks &C,
           unsigned Workers,
           size_t Window,
           size_t Budget) {
    revng_assert(Workers > 0 and Window > 0);

    std::vector<std::thread> Threads;
    for (unsigned I = 0; I < Workers; ++I)
      Threads.emplace_back([this, &C]() { work(C); });

    std::deque<std::unique_ptr<Entry>> Ahead;
    size_t InFlight = 0;
    size_t InFlightCost = 0;

    auto Release = [&](Entry &E) {
      if (E.Ticket.has_value()) {
        --InFlight;
        InFlightCost -= E.Cost;
      }
      E.Ticket.reset();
      E.Result.reset();
      E.Cost = 0;
    };

    auto Prepare = [&](Entry &E, bool AtHead) {
      ++Stats.Prepared;
      E.Ticket = C.Prepare(E.Item);
      if (not E.Ticket.has_value()) {
        E.Status = State::Done;
        return;
      }

      E.Cost = C.Cost(*E.Ticket);
      ++InFlight;
      InFlightCost += E.Cost;
      std::lock_guard Guard(Lock);
      E.Status = State::Queued;
      if (AtHead)
        Jobs.push_front(&E);
      else
        Jobs.push_back(&E);
      WorkAvailable.notify_one();
    };

    auto StatusOf = [&](Entry &E) {
      std::lock_guard Guard(Lock);
      return E.Status;
    };

    while (not Pending.empty() or not Ahead.empty()) {
      while (Ahead.size() < Window and not Pending.empty()) {
        Ahead.push_back(std::make_unique<Entry>(Pending.front()));
        Pending.pop_front();
      }

      // Commit the head as soon as it is done
      Entry &Head = *Ahead.front();
      State HeadStatus = StatusOf(Head);
      if (HeadStatus == State::Done) {
        // As in the sequential algorithm, the item leaves the queue before
        // its commit, which may insert it again
        Members.erase(Head.Item);
        bool Accepted = C.Commit(Head.Item, Head.Ticket, Head.Result);
        Release(Head);
        if (Accepted) {
          ++Stats.Committed;
          Ahead.pop_front();
        } else {
          Members.insert(Head.Item);
          ++Stats.Repeated;
          Head.Status = State::Waiting;
        }
        continue;
      }

      if (HeadStatus == State::Waiting) {
        Prepare(Head, true);
        continue;
      }

      // Prepare the first item ahead that does not depend on an earlier one
      bool Prepared = false;
      if (InFlightCost < Budget) {
        for (size_t I = 1; I < Ahead.size() and not Prepared; ++I) {
          Entry &E = *Ahead[I];
          if (StatusOf(E) != State::Waiting)
            continue;

          bool Blocked = false;
          for (size_t J = 0; J < I and not Blocked; ++J)
            Blocked = C.DependsOn(E.Item, Ahead[J]->Item);
          if (Blocked)
            continue;

          Prepare(E, false);
          Prepared = true;
        }
      }
      if (Prepared)
        continue;

      // Nothing to do but wait for a worker to finish something
      std::unique_lock Guard(Lock);
      size_t Seen = Completions;
      WorkDone.wait(Guard, [&]() {
        return Completions != Seen or Head.Status == State::Done;
      });
    }

    {
      std::lock_guard Guard(Lock);
      Stopping = true;
    }
    WorkAvailable.notify_all();
    for (std::thread &Thread : Threads)
      Thread.join();

    revng_assert(InFlight == 0 and InFlightCost == 0);
  }

private:
  void work(const Callbacks &C) {
    while (true) {
      Entry *E = nullptr;
      {
        std::unique_lock Guard(Lock);
        WorkAvailable.wait(Guard,
                           [this]() { return Stopping or not Jobs.empty(); });
        if (Jobs.empty())
          return;
        E = Jobs.front();
        Jobs.pop_front();
        E->Status = State::Running;
      }

      ResultT Result = C.Work(*E->Ticket);

      {
        std::lock_guard Guard(Lock);
        E->Result = std::move(Result);
        E->Status = State::Done;
        ++Completions;
      }
      WorkDone.notify_one();
    }
  }
};
