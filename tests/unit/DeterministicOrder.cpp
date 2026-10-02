//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DeterministicOrder
bool init_unit_test();
#include <new>
#include <utility>

#include "boost/test/unit_test.hpp"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include "revng/EarlyFunctionAnalysis/AnalyzeRegisterUsage.h"
#include "revng/PipeboxCommon/LLVMContainer.h"
#include "revng/PipeboxCommon/ObjectID.h"
#include "revng/Support/IRHelpers.h"
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

BOOST_AUTO_TEST_CASE(PrunedCompileUnitsKeepModuleOrder) {
  LLVMContext Context;
  Module M("m", Context);
  auto *Type = FunctionType::get(Type::getVoidTy(Context), false);

  // One compile unit per DIBuilder
  DIBuilder FirstBuilder(M);
  DIBuilder SecondBuilder(M);
  auto MakeUnit = [](DIBuilder &Builder, StringRef Name) {
    return Builder.createCompileUnit(dwarf::DW_LANG_C,
                                     Builder.createFile(Name, "/"),
                                     "test",
                                     false,
                                     "",
                                     0);
  };
  DICompileUnit *Low = MakeUnit(FirstBuilder, "low.c");
  DICompileUnit *High = MakeUnit(SecondBuilder, "high.c");
  if (High < Low)
    std::swap(Low, High);

  // The first function belongs to the unit with the higher address
  auto AddFunction = [&](StringRef Name, DICompileUnit *Unit) {
    auto *F = Function::Create(Type, GlobalValue::ExternalLinkage, Name, &M);
    BasicBlock::Create(Context, "", F);
    ReturnInst::Create(Context, &F->getEntryBlock());
    DIBuilder Builder(M, false, Unit);
    auto *SubroutineType = Builder
                             .createSubroutineType(Builder
                                                     .getOrCreateTypeArray({}));
    auto *Subprogram = Builder.createFunction(Unit,
                                              Name,
                                              Name,
                                              Unit->getFile(),
                                              1,
                                              SubroutineType,
                                              1,
                                              DINode::FlagZero,
                                              DISubprogram::SPFlagDefinition);
    F->setSubprogram(Subprogram);
    Builder.finalize();
  };
  AddFunction("first", High);
  AddFunction("second", Low);
  FirstBuilder.finalize();
  SecondBuilder.finalize();

  pruneDICompileUnits(M);

  NamedMDNode *Units = M.getNamedMetadata("llvm.dbg.cu");
  BOOST_TEST_REQUIRE(Units->getNumOperands() == 2U);
  BOOST_TEST(Units->getOperand(0) == High);
  BOOST_TEST(Units->getOperand(1) == Low);
}

BOOST_AUTO_TEST_CASE(StripDeadDebugInfoKeepsListOrder) {
  LLVMContext Context;
  Module M("m", Context);
  auto *Type = FunctionType::get(Type::getVoidTy(Context), false);

  DIBuilder FirstBuilder(M);
  DIBuilder SecondBuilder(M);
  DIBuilder DeadBuilder(M);
  auto MakeUnit = [](DIBuilder &Builder, StringRef Name) {
    return Builder.createCompileUnit(dwarf::DW_LANG_C,
                                     Builder.createFile(Name, "/"),
                                     "test",
                                     false,
                                     "",
                                     0);
  };
  DICompileUnit *Low = MakeUnit(FirstBuilder, "low.c");
  DICompileUnit *High = MakeUnit(SecondBuilder, "high.c");
  DICompileUnit *Dead = MakeUnit(DeadBuilder, "dead.c");
  if (High < Low)
    std::swap(Low, High);

  auto AddFunction = [&](StringRef Name, DICompileUnit *Unit) {
    auto *F = Function::Create(Type, GlobalValue::ExternalLinkage, Name, &M);
    BasicBlock::Create(Context, "", F);
    ReturnInst::Create(Context, &F->getEntryBlock());
    DIBuilder Builder(M, false, Unit);
    auto *SubroutineType = Builder
                             .createSubroutineType(Builder
                                                     .getOrCreateTypeArray({}));
    auto *Subprogram = Builder.createFunction(Unit,
                                              Name,
                                              Name,
                                              Unit->getFile(),
                                              1,
                                              SubroutineType,
                                              1,
                                              DINode::FlagZero,
                                              DISubprogram::SPFlagDefinition);
    F->setSubprogram(Subprogram);
    Builder.finalize();
  };
  AddFunction("first", High);
  AddFunction("second", Low);
  FirstBuilder.finalize();
  SecondBuilder.finalize();
  DeadBuilder.finalize();

  // List the units against their address order; the dead one makes the pass
  // rebuild the list
  NamedMDNode *Units = M.getOrInsertNamedMetadata("llvm.dbg.cu");
  Units->clearOperands();
  Units->addOperand(High);
  Units->addOperand(Low);
  Units->addOperand(Dead);

  BOOST_TEST(stripDeadDebugInfoInOrder(M));

  Units = M.getNamedMetadata("llvm.dbg.cu");
  BOOST_TEST_REQUIRE(Units->getNumOperands() == 2U);
  BOOST_TEST(Units->getOperand(0) == High);
  BOOST_TEST(Units->getOperand(1) == Low);
}

BOOST_AUTO_TEST_CASE(FunctionModulesAreParsedInObjectOrder) {
  // Two modules that both define %T: the one parsed second into the shared
  // context gets a suffixed name
  auto MakeBitcode = [](StringRef Name) {
    LLVMContext Context;
    Module M(Name, Context);
    auto *T = StructType::create(Context, { Type::getInt32Ty(Context) }, "T");
    new GlobalVariable(M,
                       T,
                       false,
                       GlobalValue::ExternalLinkage,
                       ConstantAggregateZero::get(T),
                       Name);
    SmallVector<char, 0> Buffer;
    writeBitcode(M, Buffer);
    return Buffer;
  };
  SmallVector<char, 0> FirstBitcode = MakeBitcode("first");
  SmallVector<char, 0> SecondBitcode = MakeBitcode("second");

  // The object that sorts first gets the higher address
  void *Low = ::operator new(sizeof(ObjectID));
  void *High = ::operator new(sizeof(ObjectID));
  if (High < Low)
    std::swap(Low, High);
  auto *Second = new (Low) ObjectID(MetaAddress::fromString("0x2000:Code_"
                                                            "x86_64"));
  auto *First = new (High) ObjectID(MetaAddress::fromString("0x1000:Code_"
                                                            "x86_64"));
  BOOST_TEST_REQUIRE((*First < *Second));

  revng::pypeline::LLVMFunctionContainer Container;
  Container.deserialize({ { First, FirstBitcode }, { Second, SecondBitcode } });

  auto TypeName = [&Container](const ObjectID &Object, StringRef Global) {
    const Module &M = Container.getModule(Object);
    return M.getGlobalVariable(Global)->getValueType()->getStructName().str();
  };
  BOOST_TEST(TypeName(*First, "first") == "T");
  BOOST_TEST(TypeName(*Second, "second") != "T");

  First->~ObjectID();
  Second->~ObjectID();
  ::operator delete(Low);
  ::operator delete(High);
}
