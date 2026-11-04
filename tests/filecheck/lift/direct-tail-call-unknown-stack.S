#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` jumps to `helper` with a stack pointer taken from its argument, so
# the stack analysis cannot confirm the jump is a tail call: the path is a
# long jump, and the C aborts there. The branch used to keep both its tail
# call and the long jump, and isolation aborted on a block with two exits.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: if (!argument_0) {
# CHECK: revng_abort((void *) "A longjmp was taken");

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  test %rdi, %rdi
  je 1f
  mov %rdi, %rsp
  jmp helper
1:
  ret
  .cfi_endproc
.size entry, .-entry

.p2align 4, 0xcc
.type helper, @function
helper:
  .cfi_startproc
  lea 1(%rsi), %rax
  ret
  .cfi_endproc
.size helper, .-helper
