//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE OpaqueRegisterUser
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/OpaqueRegisterUser.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

using namespace llvm;

static const char *ModuleText = R"LLVM(
@rax = global i64 0
@rbx = global i64 0
@rcx = global i64 0

declare i64 @clobber_rax()
declare i64 @clobber_rbx()

define void @f() {
  ret void
}
)LLVM";

static unsigned callsTo(Module &M, StringRef Name) {
  Function *F = M.getFunction(Name);
  return F == nullptr ? 0 : F->getNumUses();
}

BOOST_AUTO_TEST_CASE(ReusesTheTaggedFunctionsOfTheModule) {
  LLVMContext Context;
  SMDiagnostic Diagnostic;
  MemoryBufferRef Buffer(ModuleText, "module");
  std::unique_ptr<Module> M = parseIR(Buffer, Diagnostic, Context);
  revng_check(M != nullptr);
  FunctionTags::ClobbererFunction.addTo(M->getFunction("clobber_rax"));

  Function *F = M->getFunction("f");
  revng::IRBuilder Builder(Context);
  Builder.SetInsertPoint(&F->getEntryBlock().front());

  OpaqueRegisterUser First(M.get());
  OpaqueRegisterUser Second(M.get());

  // The tagged clobber_rax is reused; the untagged clobber_rbx is not, so a
  // tagged function is created beside it
  First.clobber(Builder, M->getGlobalVariable("rax"));
  First.clobber(Builder, M->getGlobalVariable("rbx"));
  First.clobber(Builder, M->getGlobalVariable("rax"));
  revng_check(callsTo(*M, "clobber_rax") == 2);
  revng_check(callsTo(*M, "clobber_rbx") == 0);
  revng_check(callsTo(*M, "clobber_rbx.1") == 1);
  revng_check(M->getFunction("clobber_rax.1") == nullptr);
  revng_check(FunctionTags::ClobbererFunction.isTagOf(M->getFunction("clobber_"
                                                                     "rbx.1")));

  // A user reuses what another user created after it was constructed
  First.clobber(Builder, M->getGlobalVariable("rcx"));
  Second.clobber(Builder, M->getGlobalVariable("rcx"));
  Second.clobber(Builder, M->getGlobalVariable("rax"));
  revng_check(callsTo(*M, "clobber_rcx") == 2);
  revng_check(M->getFunction("clobber_rcx.1") == nullptr);
  revng_check(callsTo(*M, "clobber_rax") == 3);
}
