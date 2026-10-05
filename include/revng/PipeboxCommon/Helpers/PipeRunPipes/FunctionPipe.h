#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <utility>
#include <vector>

#include "llvm/Support/Progress.h"

#include "revng/ADT/SpeculativeWorklist.h"

#include "revng/PipeboxCommon/Helpers/PipeRunPipes/Base.h"
#include "revng/PipeboxCommon/Helpers/PipeRunPipes/Helpers.h"
#include "revng/Support/CommandLine.h"
#include "revng/Support/Debug.h"

template<typename T>
concept IsFunctionPipeRun = requires(T &PipeRun,
                                     const model::Function &Function) {
  requires IsBasePipeRun<T>;
  { PipeRun.runOnFunction(Function) } -> std::same_as<void>;
};

/// A function pipe whose expensive part can run on another thread: `prepare`
/// and `finish` run on the calling thread, each in its function's own model
/// read tracking, and `work` touches nothing but its ticket
template<typename T>
concept IsShippingFunctionPipeRun = requires(T &PipeRun,
                                            const model::Function &Function,
                                            typename T::Ticket &&Ticket,
                                            typename T::Result &&Result) {
  requires IsFunctionPipeRun<T>;
  { PipeRun.prepare(Function) } -> std::same_as<typename T::Ticket>;
  {
    T::work(std::as_const(Ticket))
  } -> std::same_as<typename T::Result>;
  {
    PipeRun.finish(Function, std::move(Ticket), std::move(Result))
  } -> std::same_as<void>;
  { T::cost(std::as_const(Ticket)) } -> std::convertible_to<size_t>;
  { PipeRun.canRunInParallel() } -> std::same_as<bool>;
};

template<IsFunctionPipeRun T>
class FunctionPipe : public SingleOutputPipeBase<T> {
private:
  using Base = SingleOutputPipeBase<T>;
  static_assert(Base::OutputContainerType::Kind == Kinds::Function);

public:
  using Arguments = T::Arguments;

public:
  template<typename... Args>
    requires std::is_same_v<typename Base::ContainerTypes, TypeList<Args...>>
  revng::pypeline::PipeOutput run(const Model &Model,
                                  const revng::pypeline::Request &Incoming,
                                  const revng::pypeline::Request &Outgoing,
                                  llvm::StringRef Configuration,
                                  Args &...Containers) {
    ObjectDependenciesHelper ODH(Model, Outgoing, this->ContainerCount);

    llvm::Task T1(3, "Running " + this->Name);
    T1.advance("prologue", true);

    T Instance(Model, this->StaticConfiguration, Configuration, Containers...);
    const model::Binary &Binary = *Model.get().get();
    auto &RequestedFunctions = Outgoing.at(this->OutputContainerIndex);

    T1.advance("run on functions", true);
    llvm::Task T2(RequestedFunctions.size(), "Running pipe on functions");

    if constexpr (IsShippingFunctionPipeRun<T>) {
      unsigned Workers = FunctionPipeJobs;
      if (Workers > 1 and Instance.canRunInParallel()
          and not anyLoggerEnabled()) {
        runInParallel(Instance, Binary, RequestedFunctions, ODH, T2, Workers);
        T1.advance("epilogue");
        return { ODH.takeDependencies(), {} };
      }
    }

    for (const ObjectID *Object : RequestedFunctions) {
      const MetaAddress &Entry = std::get<MetaAddress>(Object->key());
      T2.advance(Entry.toString(), true);

      auto Committer = ODH.getCommitterFor(*Object, this->OutputContainerIndex);
      const model::Function &Function = Binary.Functions().at(Entry);
      Instance.runOnFunction(Function);
    }

    T1.advance("epilogue");
    return { ODH.takeDependencies(), {} };
  }

private:
  /// Prepare and finish each function in order on this thread, each inside a
  /// committer for that function, and run `work` on \p Workers threads. The
  /// two commits of a function add up to the reads of one `runOnFunction`.
  template<typename Requested>
  void runInParallel(T &Instance,
                     const model::Binary &Binary,
                     const Requested &RequestedFunctions,
                     ObjectDependenciesHelper &ODH,
                     llvm::Task &Progress,
                     unsigned Workers)
    requires IsShippingFunctionPipeRun<T>
  {
    using Ticket = typename T::Ticket;
    using Result = typename T::Result;
    using Worklist = SpeculativeWorklist<const ObjectID *, Ticket, Result>;

    Worklist Functions;
    for (const ObjectID *Object : RequestedFunctions)
      Functions.insert(Object);

    auto FunctionOf = [&](const ObjectID *Object) -> const model::Function & {
      return Binary.Functions().at(std::get<MetaAddress>(Object->key()));
    };

    typename Worklist::Callbacks C;
    C.Prepare = [&](const ObjectID *Object) -> std::optional<Ticket> {
      auto Committer = ODH.getCommitterFor(*Object, this->OutputContainerIndex);
      return Instance.prepare(FunctionOf(Object));
    };
    C.Cost = [](const Ticket &TheTicket) -> size_t {
      return T::cost(TheTicket);
    };
    C.Work = [](const Ticket &TheTicket) { return T::work(TheTicket); };
    C.Commit = [&](const ObjectID *Object,
                   std::optional<Ticket> &TheTicket,
                   std::optional<Result> &TheResult) {
      Progress.advance(std::get<MetaAddress>(Object->key()).toString(), true);
      auto Committer = ODH.getCommitterFor(*Object, this->OutputContainerIndex);
      Instance.finish(FunctionOf(Object),
                      std::move(*TheTicket),
                      std::move(*TheResult));
      return true;
    };
    C.Reads = [](const ObjectID *) { return std::vector<const ObjectID *>(); };

    Functions.run(C, Workers, 16 * Workers, 8'000'000);
  }
};
