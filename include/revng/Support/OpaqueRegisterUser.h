#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Model/Register.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/OpaqueFunctionsPool.h"

namespace revng {
class IRBuilder;
} // namespace revng

class OpaqueRegisterUser {
private:
  llvm::Module *M;
  OpaqueFunctionsPool<std::string> Clobberers;
  OpaqueFunctionsPool<std::string> Writers;
  OpaqueFunctionsPool<std::string> Readers;
  llvm::SmallVector<llvm::Instruction *, 16> Created;

public:
  OpaqueRegisterUser(llvm::Module *M) :
    M(M), Clobberers(M, false), Writers(M, false), Readers(M, false) {
    using namespace llvm;

    Clobberers.setMemoryEffects(MemoryEffects::readOnly());
    Clobberers.addFnAttribute(Attribute::NoUnwind);
    Clobberers.addFnAttribute(Attribute::WillReturn);
    Clobberers.setTags({ &FunctionTags::ClobbererFunction });

    Writers.setMemoryEffects(MemoryEffects::readOnly());
    Writers.addFnAttribute(Attribute::NoUnwind);
    Writers.addFnAttribute(Attribute::WillReturn);
    Writers.setTags({ &FunctionTags::WriterFunction });

    Readers.setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
    Readers.addFnAttribute(Attribute::NoUnwind);
    Readers.addFnAttribute(Attribute::WillReturn);
    Readers.setTags({ &FunctionTags::ReaderFunction });
  }

public:
  void clobber(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    writeImpl(Builder,
              CSV,
              "clobber_",
              Clobberers,
              FunctionTags::ClobbererFunction);
  }

  void clobber(revng::IRBuilder &Builder, model::Register::Values Value) {
    for (const model::Register::CSV &CSV : model::Register::getCSVs(Value))
      if (auto *Variable = M->getGlobalVariable(CSV.Name))
        clobber(Builder, Variable);
  }

  llvm::StoreInst *write(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    return writeImpl(Builder,
                     CSV,
                     "write_",
                     Writers,
                     FunctionTags::WriterFunction);
  }

  llvm::Instruction *read(revng::IRBuilder &Builder,
                          llvm::GlobalVariable *CSV) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = "reader_" + CSV->getName().str();
    adopt(Readers, FunctionTags::ReaderFunction, Name);
    llvm::Function *Reader = Readers.get(Name,
                                         Builder.getVoidTy(),
                                         { CSVTy },
                                         Name);

    auto *Load = Builder.CreateLoad(CSVTy, CSV);
    auto *OpaqueCall = Builder.CreateCall(Reader, Load);

    Created.push_back(OpaqueCall);
    Created.push_back(Load);

    return OpaqueCall;
  }

  void purgeCreated() {
    for (llvm::Instruction *I : Created)
      I->eraseFromParent();
    Created.clear();
  }

private:
  /// Let \p Pool reuse the function named \p Name if the module already has
  /// it with \p Tag. Looking each name up when it is needed, rather than
  /// recording every tagged function of the module up front, keeps this class
  /// cheap to construct in a module with many functions; passes construct one
  /// per function.
  void adopt(OpaqueFunctionsPool<std::string> &Pool,
             const FunctionTags::Tag &Tag,
             const std::string &Name) {
    if (llvm::Function *F = M->getFunction(Name))
      if (Tag.isTagOf(F))
        Pool.record(Name, F);
  }

  llvm::StoreInst *writeImpl(revng::IRBuilder &Builder,
                             llvm::GlobalVariable *CSV,
                             llvm::StringRef Prefix,
                             OpaqueFunctionsPool<std::string> &Pool,
                             const FunctionTags::Tag &Tag) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = Prefix.str() + CSV->getName().str();
    adopt(Pool, Tag, Name);
    llvm::Function *Writer = Pool.get(Name, CSVTy, {}, Name);

    auto *OpaqueCall = Builder.CreateCall(Writer);
    auto *Store = Builder.CreateStore(OpaqueCall, CSV);

    Created.push_back(Store);
    Created.push_back(OpaqueCall);

    return Store;
  }
};
