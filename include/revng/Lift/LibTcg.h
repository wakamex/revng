#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "qemu/libtcg/libtcg.h"

#include "revng/Model/Architecture.h"

class LibTcg {
public:
  class TranslationBlock {
  private:
    LibTcgInterface &Interface;
    LibTcgContext &Context;
    LibTcgTranslationBlock Block;

  public:
    TranslationBlock(LibTcgInterface &Interface,
                     LibTcgContext &Context,
                     LibTcgTranslationBlock Block) :
      Interface(Interface), Context(Context), Block(Block) {}

    ~TranslationBlock() {
      Interface.translation_block_destroy(&Context, Block);
    }

  public:
    LibTcgTranslationBlock &operator*() { return Block; }
    const LibTcgTranslationBlock &operator*() const { return Block; }
    LibTcgTranslationBlock *operator->() { return &Block; }
    const LibTcgTranslationBlock *operator->() const { return &Block; }
  };

private:
  void *LibraryHandle = nullptr;
  LibTcgInterface Interface;
  LibTcgContext *Context = nullptr;
  LibTcgArchInfo ArchInfo;
  std::map<intptr_t, llvm::StringRef> GlobalNames;

  // QEMU's `page_get_flags`, which libtcg exports. Its argument is a guest
  // address (`target_ulong`), which is never wider than 64 bits.
  using PageGetFlagsFunc = int(uint64_t);
  PageGetFlagsFunc *PageGetFlags = nullptr;
  uint64_t MaxGuestAddress = 0;

public:
  ~LibTcg();

public:
  static LibTcg get(model::Architecture::Values Architecture);

public:
  const LibTcgArchInfo &archInfo() const { return ArchInfo; }

  uint8_t *envPointer() { return Interface.env_ptr(Context); }

  TranslationBlock translateBlock(const unsigned char *Buffer,
                                  size_t Size,
                                  uint64_t VirtualAddress,
                                  uint32_t TranslateFlags) {
    return TranslationBlock(Interface,
                            *Context,
                            Interface.translate_block(Context,
                                                      Buffer,
                                                      Size,
                                                      VirtualAddress,
                                                      TranslateFlags));
  }

  void dumpInstructionToBuffer(LibTcgInstruction *Instruction,
                               char *Buffer,
                               size_t Size) {
    Interface.dump_instruction_to_buffer(Instruction, Buffer, Size);
  }

  const char *instructionName(LibTcgOpcode Opcode) {
    return Interface.get_instruction_name(Opcode);
  }

  LibTcgHelperInfo helperInfo(LibTcgInstruction *InstructionInfo) {
    return Interface.get_helper_info(InstructionInfo);
  }

  const auto &globalNames() const { return GlobalNames; }

  /// Whether \p Address is a guest address of memory that libtcg mapped.
  ///
  /// libtcg sets up its CPU the way QEMU's linux-user does, and that setup
  /// maps guest memory for parts of the CPU state, such as the x86 descriptor
  /// tables, and stores their address in the CPU state. libtcg reserves no
  /// guest address space, so these addresses are wherever the host placed the
  /// mapping: they change with the memory layout of the process.
  bool isMappedGuestAddress(uint64_t Address) const;
};
