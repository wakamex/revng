//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DeterministicOrder
bool init_unit_test();
#include <new>
#include <utility>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include "revng/EarlyFunctionAnalysis/AnalyzeRegisterUsage.h"
#include "revng/Support/Tag.h"

using namespace llvm;

// Each test builds two objects, looks at which one got the lower address and
// arranges them so that the expected order is the opposite of the order by
// address. A container whose order follows addresses then gets it wrong.

/// Two tags whose names sort opposite to their addresses. They are created
/// during static initialization, before the tag hierarchy is frozen.
static std::pair<FunctionTags::Tag *, FunctionTags::Tag *> makeTags() {
  using FunctionTags::Tag;
  void *Low = ::operator new(sizeof(Tag));
  void *High = ::operator new(sizeof(Tag));
  if (High < Low)
    std::swap(Low, High);
  auto *Second = new (Low) Tag("test-deterministic-order-z");
  auto *First = new (High) Tag("test-deterministic-order-a");
  return { First, Second };
}

static const auto Tags = makeTags();

BOOST_AUTO_TEST_CASE(TagsSetIsOrderedByName) {
  auto &&[First, Second] = Tags;
  BOOST_TEST_REQUIRE(static_cast<void *>(Second) < static_cast<void *>(First));

  LLVMContext Context;
  Module M("m", Context);
  auto *Type = FunctionType::get(Type::getVoidTy(Context), false);
  auto *F = Function::Create(Type, GlobalValue::ExternalLinkage, "f", &M);

  FunctionTags::TagsSet Set;
  Set.insert(*Second);
  Set.insert(*First);
  Set.set(F);

  auto *MD = cast<MDTuple>(F->getMetadata("revng.tags"));
  BOOST_TEST_REQUIRE(MD->getNumOperands() == 2U);
  auto Name = [&MD](unsigned Index) {
    return cast<MDString>(MD->getOperand(Index))->getString().str();
  };
  BOOST_TEST(Name(0) == "test-deterministic-order-a");
  BOOST_TEST(Name(1) == "test-deterministic-order-z");
}

BOOST_AUTO_TEST_CASE(CSVSetIsOrderedByName) {
  LLVMContext Context;
  Module M("m", Context);
  auto *Int64 = Type::getInt64Ty(Context);
  auto MakeCSV = [&]() {
    return new GlobalVariable(M,
                              Int64,
                              false,
                              GlobalValue::ExternalLinkage,
                              ConstantInt::get(Int64, 0));
  };
  GlobalVariable *Low = MakeCSV();
  GlobalVariable *High = MakeCSV();
  if (High < Low)
    std::swap(Low, High);
  Low->setName("_z");
  High->setName("_a");

  efa::CSVSet Set{ Low, High, nullptr };
  BOOST_TEST_REQUIRE(Set.size() == 3U);
  auto It = Set.begin();
  BOOST_TEST(*It++ == nullptr);
  BOOST_TEST(*It++ == High);
  BOOST_TEST(*It++ == Low);

  // Looking up a register that has no CSV must work
  BOOST_TEST(Set.count(nullptr) == 1U);
}
