#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` frees each pointer of an array through `free`'s GOT slot, which it
# loads into a register before the loop, as compilers do for a call in a loop
# to an imported function. When lifting, the register's value at the call is
# unknown from the second iteration on; detect-abi, which keeps a register the
# callee preserves, finds the call's target loaded from the slot that `free`'s
# relocation fills, so the C calls `free` instead of a pointer read from the
# GOT.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared -s --build-id=none --no-relax -z relro %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: .got
# CHECK: do {
# CHECK: free(
# CHECK: } while

.text
.type free, @function
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
  mov %rsi, %r13
  mov free@GOTPCREL(%rip), %r12
1:
  mov (%rbx), %rdi
  call *%r12
  add $8, %rbx
  dec %r13
  jnz 1b
  pop %r13
  .cfi_def_cfa_offset 24
  pop %r12
  .cfi_def_cfa_offset 16
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry
