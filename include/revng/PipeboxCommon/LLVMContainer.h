#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/ObjectID.h"
#include "revng/Support/IRHelpers.h"

namespace revng::pypeline {

class LLVMRootContainer {
public:
  static constexpr llvm::StringRef Name = "LLVMRootContainer";
  static constexpr Kind Kind = Kinds::Binary;
  static constexpr llvm::StringRef MimeType = "application/x.llvm.bc";
  static constexpr llvm::StringRef Compression = "zstd;level=3";

private:
  bool Disposable = false;
  std::optional<llvm::LLVMContext> Context;
  // NOTE: this cannot grouped with Context because most of the LLVM APIs
  // return a `std::unique_ptr<llvm::Module>`
  std::unique_ptr<llvm::Module> Module;

public:
  LLVMRootContainer() : Context(std::in_place_t{}) {
    Module = std::make_unique<llvm::Module>("revng.module", *Context);
  }

public:
  std::set<ObjectID> objects() const {
    if (Module->empty())
      return std::set<ObjectID>{};
    else
      return std::set{ ObjectID() };
  }

  void
  deserialize(const std::map<const ObjectID *, llvm::ArrayRef<char>> Data) {
    if (Data.size() == 0)
      return;

    revng_assert(Data.size() == 1);
    for (const auto &[Object, Buffer] : Data) {
      revng_assert(Object->kind() == Kind);
      llvm::MemoryBufferRef Ref{ { Buffer.data(), Buffer.size() }, "input" };
      Module = llvm::cantFail(llvm::parseBitcodeFile(Ref, *Context));
    }
  }

  std::map<ObjectID, Buffer>
  serialize(const std::vector<const ObjectID *> Objects) const {
    if (Objects.size() == 0)
      return {};

    revng_assert(Objects.size() == 1 and Objects[0]->kind() == Kind);
    std::map<ObjectID, Buffer> Result;
    writeBitcode(*Module, Result[*Objects[0]].data());

    return Result;
  }

  bool verify() const {
    revng::forceVerify(&*Module);
    return true;
  }

  void setIsDisposable() { Disposable = true; }

  void disposeIfPossible() {
    if (not Disposable)
      return;

    Module.reset();
    Context.emplace();
    Module = std::make_unique<llvm::Module>("revng.module", *Context);
    Disposable = false;
  }

public:
  const llvm::Module &getModule() const { return *Module; }
  llvm::Module &getModule() { return *Module; }

  void assign(std::unique_ptr<llvm::Module> &&NewModule) {
    if (&*Context == &NewModule->getContext())
      Module = std::move(NewModule);
    else
      Module = cloneIntoContext(*NewModule, *Context);
  }
};

class LLVMFunctionContainer {
public:
  static constexpr llvm::StringRef Name = "LLVMFunctionContainer";
  static constexpr Kind Kind = Kinds::Function;
  static constexpr llvm::StringRef MimeType = "application/x.llvm.bc";
  static constexpr llvm::StringRef Compression = "zstd;level=2";

  using Bitcode = llvm::SmallVector<char, 0>;

private:
  bool Disposable = false;

  // Each function is either a module in `Context` or, until something asks
  // for its module, bitcode: deserializing keeps bitcode, and so do pipes
  // that run on other threads, which take and return bitcode. Parsing
  // happens on demand, so a const accessor can parse.
  mutable std::optional<llvm::LLVMContext> Context;
  mutable std::map<ObjectID, std::unique_ptr<llvm::Module>> Modules;
  mutable std::map<ObjectID, Bitcode> Unparsed;

public:
  LLVMFunctionContainer() : Context(std::in_place_t{}) {}

public:
  std::set<ObjectID> objects() const {
    std::set<ObjectID> Result = std::views::keys(Modules)
                                | revng::to<std::set<ObjectID>>();
    for (const auto &[Object, _] : Unparsed)
      Result.insert(Object);
    return Result;
  }

  void
  deserialize(const std::map<const ObjectID *, llvm::ArrayRef<char>> Data) {
    for (auto const &[Object, Buffer] : Data) {
      Modules.erase(*Object);
      Unparsed[*Object].assign(Buffer.begin(), Buffer.end());
    }
  }

  std::map<ObjectID, Buffer>
  serialize(const std::vector<const ObjectID *> Objects) const {
    std::map<ObjectID, Buffer> Result;
    for (const ObjectID *Object : Objects) {
      if (auto It = Unparsed.find(*Object); It != Unparsed.end())
        Result[*Object].data().assign(It->second.begin(), It->second.end());
      else
        writeBitcode(*Modules.at(*Object), Result[*Object].data());
    }
    return Result;
  }

  bool verify() const {
    for (const ObjectID &Object : objects())
      revng::forceVerify(&getModule(Object));
    return true;
  }

  void setIsDisposable() { Disposable = true; }

  void disposeIfPossible() {
    if (not Disposable)
      return;

    Modules.clear();
    Unparsed.clear();
    Context.emplace();
    Disposable = false;
  }

public:
  llvm::LLVMContext &getContext() { return *Context; }
  const llvm::LLVMContext &getContext() const { return *Context; }

  const llvm::Module &getModule(const ObjectID &ID) const {
    return *parsed(ID);
  }

  llvm::Module &getModule(const ObjectID &ID) { return *parsed(ID); }

  std::unique_ptr<llvm::Module> takeModule(const ObjectID &ID) {
    return std::move(parsed(ID));
  }

  void assign(const ObjectID &ID, std::unique_ptr<llvm::Module> &&NewModule) {
    Unparsed.erase(ID);
    if (&*Context == &NewModule->getContext())
      Modules[ID] = std::move(NewModule);
    else
      Modules[ID] = cloneIntoContext(*NewModule, *Context);
  }

  /// Remove \p ID's function and return it as bitcode, without parsing it if
  /// it is still bitcode
  Bitcode takeBitcode(const ObjectID &ID) {
    if (auto It = Unparsed.find(ID); It != Unparsed.end()) {
      Bitcode Result = std::move(It->second);
      Unparsed.erase(It);
      return Result;
    }

    auto It = Modules.find(ID);
    revng_assert(It != Modules.end() and It->second != nullptr);
    Bitcode Result;
    writeBitcode(*It->second, Result);
    Modules.erase(It);
    return Result;
  }

  /// Set \p ID's function to \p NewBitcode, parsed only when needed
  void assignBitcode(const ObjectID &ID, Bitcode &&NewBitcode) {
    Modules.erase(ID);
    Unparsed[ID] = std::move(NewBitcode);
  }

private:
  std::unique_ptr<llvm::Module> &parsed(const ObjectID &ID) const {
    if (auto It = Unparsed.find(ID); It != Unparsed.end()) {
      Modules[ID] = readBitcode(It->second, *Context);
      Unparsed.erase(It);
    }
    return Modules.at(ID);
  }
};

} // namespace revng::pypeline
