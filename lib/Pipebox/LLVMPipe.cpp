//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <memory>
#include <optional>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/PassRegistry.h"

#include "revng/ADT/SpeculativeWorklist.h"
#include "revng/Pipebox/LLVMPipe.h"
#include "revng/Support/CommandLine.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

namespace rpp = revng::pypeline::pipes;
using Configuration = rpp::detail::PureLLVMPassesPipeBase::Configuration;

template<>
struct llvm::yaml::MappingTraits<Configuration> {
  static void mapping(IO &IO, Configuration &Fields) {
    IO.mapRequired("passes", Fields.Passes);
  }
};

Configuration Configuration::parse(llvm::StringRef Input) {
  return llvm::cantFail(fromString<Configuration>(Input));
}

namespace revng::pypeline::pipes::detail {

PureLLVMPassesPipeBase::PureLLVMPassesPipeBase(llvm::StringRef
                                                 StaticConfiguration) :
  StaticConfiguration(StaticConfiguration) {

  Configuration Configuration = Configuration::parse(StaticConfiguration);
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
  for (llvm::StringRef PassName : Configuration.Passes) {
    const llvm::PassInfo *PassInfo = Registry.getPassInfo(PassName);
    revng_assert(PassInfo != nullptr,
                 ("llvm-pipe: Requested pass " + PassName
                  + " was not found in the PassRegistry")
                   .str()
                   .c_str());
    PassInfos.push_back(PassInfo);
  }
  TaskName = "PureLLVMPasses(" + llvm::join(Configuration.Passes, ",") + ")";
}

} // namespace revng::pypeline::pipes::detail

namespace revng::pypeline::pipes {

namespace {

/// A worker's context, and the passes it runs there. Workers are threads of
/// one run, and both live as long as the thread, so passes that load
/// something once per context, such as the helpers `link-helpers-to-inline`
/// links, do so once per worker.
struct PassesWorker {
  llvm::LLVMContext Context;
  llvm::legacy::PassManager Manager;
};

thread_local std::unique_ptr<PassesWorker> Worker;

} // namespace

bool PureLLVMPassesPipe::runInParallel(const std::vector<const ObjectID *>
                                         &Objects,
                                       LLVMFunctionContainer &Container,
                                       llvm::Task &Progress) const {
  unsigned Workers = FunctionPipeJobs;
  if (Workers <= 1 or Objects.size() <= 1 or anyLoggerEnabled())
    return false;

  using Bitcode = LLVMFunctionContainer::Bitcode;
  SpeculativeWorklist<const ObjectID *, Bitcode, Bitcode> Functions;
  for (const ObjectID *Object : Objects) {
    revng_assert(Object->kind() == Kinds::Function);
    Functions.insert(Object);
  }

  using Worklist = decltype(Functions);
  Worklist::Callbacks C;
  C.Prepare = [&](const ObjectID *Object) -> std::optional<Bitcode> {
    return Container.takeBitcode(*Object);
  };
  C.Cost = [](const Bitcode &Input) -> size_t { return Input.size(); };
  C.Work = [this](const Bitcode &Input) {
    if (Worker == nullptr) {
      Worker = std::make_unique<PassesWorker>();
      addPasses(Worker->Manager);
    }
    std::unique_ptr<llvm::Module> Module = readBitcode(Input,
                                                       Worker->Context);
    Worker->Manager.run(*Module);

    // A struct's name in a context depends on the names other types there
    // already hold, here on which functions this worker ran before: write
    // the function with unnamed structs, so its bitcode depends only on its
    // content. Nothing that runs on single functions looks a type up by name.
    for (llvm::StructType *Type : Module->getIdentifiedStructTypes())
      Type->setName("");
    Bitcode Output;
    writeBitcode(*Module, Output);
    return Output;
  };
  C.Commit = [&](const ObjectID *Object,
                 std::optional<Bitcode> &,
                 std::optional<Bitcode> &Output) {
    Progress.advance(std::get<MetaAddress>(Object->key()).toString(), true);
    Container.assignBitcode(*Object, std::move(*Output));
    return true;
  };
  C.Reads = [](const ObjectID *) { return std::vector<const ObjectID *>(); };

  // Bitcode is about two bytes per instruction: keep a few million
  // instructions in flight
  Functions.run(C, Workers, 16 * Workers, 16'000'000);
  return true;
}

} // namespace revng::pypeline::pipes
