/// Collect the function entry points from unused addresses.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "boost/icl/interval_set.hpp"
#include "boost/icl/right_open_interval.hpp"

#include "llvm/IR/Module.h"

#include "revng/EarlyFunctionAnalysis/CollectFunctionsFromUnusedAddresses.h"
#include "revng/EarlyFunctionAnalysis/ControlFlowGraphCache.h"
#include "revng/Lift/JumpTargetReason.h"
#include "revng/Lift/Lift.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/NewPC.h"

using namespace llvm;

static Logger Log("functions-from-unused-addresses-collection");

class CFFUAImpl {
public:
  CFFUAImpl(llvm::Module &M, RootFunction &Root, model::Binary &Binary) :
    M(M), Root(Root), Binary(Binary) {}

  void run(ControlFlowGraphCache &MDCache) {
    loadAllCFGs(MDCache);
    collectFunctionsFromUnusedAddresses();
  }

private:
  void loadAllCFGs(ControlFlowGraphCache &MDCache) {
    for (auto &Function : Binary.Functions()) {
      llvm::BasicBlock *Entry = Root.getBlockAt(Function.Entry());
      llvm::Instruction *Term = Entry->getTerminator();

      const efa::ControlFlowGraph &FM = MDCache.getControlFlowGraph(Function
                                                                      .Entry());
      for (const efa::BasicBlock &Block : FM.Blocks()) {
        auto Start = Block.ID().start();
        auto End = Block.End();
        revng_log(Log,
                  "Registering as used range [" << Start.toString() << ", "
                                                << End.toString() << ")");
        UsedRanges += interval::right_open(Start, End);
      }
    }
  }

  void collectFunctionsFromUnusedAddresses() {
    using namespace llvm;
    Function &Root = *M.getFunction("root");

    for (BasicBlock &BB : Root) {
      if (getType(&BB) != BlockType::JumpTargetBlock)
        continue;

      MetaAddress Entry = getBasicBlockAddress(getJumpTargetBlock(&BB));
      if (Binary.Functions().tryGet(Entry) != nullptr)
        continue;

      uint32_t Reasons = JumpTargetReason::getReasons(&BB);
      bool IsSimpleLiteral = hasReason(Reasons,
                                       JumpTargetReason::SimpleLiteral);
      bool IsGlobalData = hasReason(Reasons, JumpTargetReason::GlobalData);
      bool IsUnusedGlobalData = hasReason(Reasons,
                                          JumpTargetReason::UnusedGlobalData);
      bool IsDirectJump = hasReason(Reasons, JumpTargetReason::DirectJump);
      bool IsMemoryStore = hasReason(Reasons, JumpTargetReason::MemoryStore);
      bool IsPCStore = hasReason(Reasons, JumpTargetReason::PCStore);
      bool IsReturnAddress = hasReason(Reasons,
                                       JumpTargetReason::ReturnAddress);
      bool IsLoadAddress = hasReason(Reasons, JumpTargetReason::LoadAddress);
      bool IsUnwindEntry = hasReason(Reasons, JumpTargetReason::UnwindEntry);
      bool IsPartOfOtherCFG = UsedRanges.find(Entry) != UsedRanges.end();

      revng_log(Log, Entry.toString()
                         << "\n"
                         << "  IsSimpleLiteral: " << IsSimpleLiteral << "\n"
                         << "  IsUnusedGlobalData: " << IsUnusedGlobalData
                         << "\n"
                         << "  IsDirectJump: " << IsDirectJump << "\n"
                         << "  IsMemoryStore: " << IsMemoryStore << "\n"
                         << "  IsPCStore: " << IsPCStore << "\n"
                         << "  IsReturnAddress: " << IsReturnAddress << "\n"
                         << "  IsLoadAddress: " << IsLoadAddress << "\n"
                         << "  IsPartOfOtherCFG: " << IsPartOfOtherCFG << "\n"
                         << "  IsUnwindEntry: " << IsUnwindEntry << "\n");

      // Look for simple literals, unused global data or address stored in
      // memory that are never written directly to the PC. Also, ignore code for
      // which we already have a direct jump or is part of the CFG of another
      // function. Also, ignore addresses that are return addresses.
      // A pointer in data to the start of a function in the unwind
      // information, such as a method in a vtable or a function in a GOT slot,
      // is a function entry even when code loads it.
      bool Interesting = (IsSimpleLiteral or IsUnusedGlobalData
                          or (IsMemoryStore and not IsPCStore)
                          or (IsGlobalData and IsUnwindEntry));
      // A pointer to the start of a function in the unwind information is a
      // function entry even when another function's CFG reaches it by falling
      // through, as one does past a call it does not know to be no-return, or
      // when a tail call jumps to it.
      bool Ignore = IsReturnAddress or
                    (not IsUnwindEntry and (IsPartOfOtherCFG or IsDirectJump));
      if (Interesting and not Ignore) {

        if (IsLoadAddress and not IsUnwindEntry) {
          revng_log(Log,
                    "Ignoring " << Entry.toString()
                                << " since it's target of a memory operation");

        } else {
          if (IsMemoryStore and not IsUnusedGlobalData) {
            revng_log(Log,
                      "Creating function due to memory store at "
                        << Entry.toString());
          }

          Binary.Functions()[Entry];
          revng_log(Log,
                    "Found function from unused addresses: "
                      << BB.getName().str());
        }
      }
    }
  }

private:
  using interval_set = boost::icl::interval_set<MetaAddress>;
  using interval = boost::icl::interval<MetaAddress>;

private:
  llvm::Module &M;
  RootFunction &Root;
  model::Binary &Binary;
  interval_set UsedRanges;
};

void collectFunctionsFromUnusedAddresses(llvm::Module &M,
                                         RootFunction &Root,
                                         model::Binary &Binary,
                                         ControlFlowGraphCache &FMC) {
  CFFUAImpl Impl(M, Root, Binary);
  Impl.run(FMC);
}
