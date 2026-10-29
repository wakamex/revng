#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` loads `work`'s GOT slot into a register once and calls through the
# register in a loop, as compilers do for a call in a loop to a function of
# another crate. When lifting, the register's value at the call is unknown
# from the second iteration on; detect-abi, which keeps a register the callee
# preserves, finds the call's target loaded from the slot. The loader makes
# the slot read-only once it has applied its relocation (RELRO), so the call
# can only reach `work` and becomes a direct call. Without RELRO the program
# could change the slot, so the call stays indirect.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --no-relax -z relro -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
# RUN: ld.lld -pie -s --build-id=none --no-relax -z norelro -e entry %t.o -o %t.norelro
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t.norelro -o %t.norelro.ptml
# RUN: %root/bin/revng ptml --plain %t.norelro.ptml | FileCheck --check-prefix=NORELRO %s
#
# CHECK-NOT: .got
# CHECK: do {
# CHECK: function_0x[[WORK:[0-9a-f]+]]_Code_x86_64(
# CHECK: } while
# CHECK: function_0x[[WORK]]_Code_x86_64(generic64_t argument_0) {
#
# NORELRO: segment_{{[0-9]+}}.got
# NORELRO: do {

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  push %r12
  .cfi_def_cfa_offset 24
  push %r13
  .cfi_def_cfa_offset 32
  mov %rdi, %rbx
  xor %r13d, %r13d
  mov work@GOTPCREL(%rip), %r12
1:
  mov %rbx, %rdi
  call *%r12
  add %rax, %r13
  dec %rbx
  jnz 1b
  mov %r13, %rax
  pop %r13
  .cfi_def_cfa_offset 24
  pop %r12
  .cfi_def_cfa_offset 16
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

.type work, @function
work:
  .cfi_startproc
  lea 1(%rdi,%rdi), %rax
  ret
  .cfi_endproc
.size work, .-work
