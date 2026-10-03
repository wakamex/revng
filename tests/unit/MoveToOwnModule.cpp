//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE MoveToOwnModule
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"

#include "revng/Support/Assert.h"
#include "revng/Support/MoveToOwnModule.h"

using namespace llvm;

// The use lists of %x, of the block %join and of @rax are not in the order
// the bitcode reader would give them. The uses of @rax by @f come first, as
// the uses of a newly outlined function do, then the one by @other. @table is
// a constant whose loads fold only if its initializer comes along.
static const char *Source = R"LLVM(
target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@rsp = global i64 0
@rax = global i64 0
@table = constant [4 x i64] [i64 10, i64 20, i64 30, i64 40]
@unused = global i64 7

declare !tag !0 void @clobber(i64)
declare i64 @opaque(i64) nounwind willreturn memory(none)

define i64 @f(i64 %a, i1 %c) {
entry:
  %sp = load i64, ptr @rsp
  %x = add i64 %a, %sp
  %y = add i64 %x, %x
  %g = getelementptr [4 x i64], ptr @table, i64 0, i64 2
  %t = load i64, ptr %g
  store i64 %y, ptr @rax
  br i1 %c, label %left, label %right

left:
  %l = call i64 @opaque(i64 %x)
  br label %join

right:
  %r = mul i64 %x, %t
  call void @clobber(i64 %r)
  br label %join

join:
  %p = phi i64 [ %l, %left ], [ %r, %right ]
  %z = add i64 %p, %y
  %w = load i64, ptr @rax
  %s = add i64 %z, %w
  store i64 %s, ptr @rsp
  ret i64 %s

  uselistorder i64 %x, { 3, 1, 0, 2 }
  uselistorder label %join, { 1, 0 }
}

define void @other() {
  store i64 1, ptr @rax
  ret void
}

uselistorder ptr @rax, { 2, 1, 0 }

!0 = !{!"tag-value"}
)LLVM";

static std::unique_ptr<Module> parse(LLVMContext &Context) {
  SMDiagnostic Error;
  auto Result = parseIR(MemoryBufferRef(Source, "source"), Error, Context);
  if (not Result) {
    Error.print("MoveToOwnModule", errs());
    revng_abort();
  }
  return Result;
}

static std::string print(const Function &F) {
  std::string Result;
  raw_string_ostream Stream(Result);
  F.print(Stream, nullptr, /* ShouldPreserveUseListOrder */ true);
  return Result;
}

static std::vector<std::string> usersOf(Module &M, StringRef Name) {
  std::vector<std::string> Result;
  for (User *U : M.getNamedValue(Name)->users())
    if (auto *I = dyn_cast<Instruction>(U))
      Result.push_back(I->getFunction()->getName().str() + ":"
                       + I->getOpcodeName());
  return Result;
}

static void optimize(Function &F) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());
  FPM.addPass(GVNPass());

  ModuleAnalysisManager MAM;
  FunctionAnalysisManager FAM;
  FAM.registerPass([] {
    AAManager AA;
    AA.registerFunctionAnalysis<BasicAA>();
    return AA;
  });
  FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(MAM); });
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerModuleAnalyses(MAM);
  FPM.run(F, FAM);
}

BOOST_AUTO_TEST_CASE(RoundTripKeepsTheFunctionAndItsUseListOrder) {
  LLVMContext Context;
  auto M = parse(Context);
  Function &F = *M->getFunction("f");
  std::string Before = print(F);
  std::vector<std::string> RaxUsers;
  for (std::string &User : usersOf(*M, "rax"))
    if (StringRef(User).startswith("f:"))
      RaxUsers.push_back(User);
  BOOST_TEST_REQUIRE(RaxUsers.size() == 2U);

  GlobalValue *Unused = M->getNamedValue("unused");
  ShippedFunction Shipped = shipFunction(F, { Unused });
  BOOST_TEST(M->getFunction("f") == nullptr);
  BOOST_TEST(M->getFunction("other") != nullptr);

  LLVMContext WorkerContext;
  auto Received = receiveFunction(Shipped, WorkerContext);
  BOOST_TEST(print(*Received->getFunction("f")) == Before);
  BOOST_TEST(usersOf(*Received, "rax") == RaxUsers);

  // Definitions, metadata and the extra global come along
  auto *Table = Received->getGlobalVariable("table", true);
  BOOST_TEST_REQUIRE(Table != nullptr);
  BOOST_TEST(Table->hasInitializer());
  auto *Clobber = Received->getFunction("clobber");
  BOOST_TEST_REQUIRE(Clobber != nullptr);
  BOOST_TEST(Clobber->getMetadata("tag") != nullptr);
  auto *ExtraGlobal = Received->getGlobalVariable("unused");
  BOOST_TEST_REQUIRE(ExtraGlobal != nullptr);
  BOOST_TEST(ExtraGlobal->hasInitializer());

  // Without the shipped order the function comes back in another order
  ShippedFunction Unordered = Shipped;
  Unordered.Order = {};
  LLVMContext OtherContext;
  auto Plain = receiveFunction(Unordered, OtherContext);
  BOOST_TEST(print(*Plain->getFunction("f")) != Before);
}

BOOST_AUTO_TEST_CASE(PipelineGivesTheSameResultAsInPlace) {
  LLVMContext Context;
  auto InPlace = parse(Context);
  optimize(*InPlace->getFunction("f"));
  std::string Expected = print(*InPlace->getFunction("f"));

  // The load from @table folded, which needs its initializer
  BOOST_TEST(Expected.find("@table") == std::string::npos);

  auto M = parse(Context);
  ShippedFunction Shipped = shipFunction(*M->getFunction("f"));
  LLVMContext WorkerContext;
  auto Received = receiveFunction(Shipped, WorkerContext);
  optimize(*Received->getFunction("f"));
  BOOST_TEST(print(*Received->getFunction("f")) == Expected);
}
