#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `relay` calls the pointer it receives in `rdi`. On one path it first copies
# 8 bytes through `xmm1`, which zeroes the rest of the register; nothing reads
# that afterwards. A call through a pointer takes the registers written before
# it and not read again, but only general purpose ones: a vector register is
# left so by every copy through it. The call used to take `zmm0` and `zmm1`.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: {{register_rdi|argument_0}})(
# CHECK-NOT: generic512_t
# CHECK-SAME: );

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  call relay
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

.type relay, @function
relay:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  test %rsi, %rsi
  je 1f
  movq (%rsi), %xmm1
  movq %xmm1, (%rdx)
1:
  call *%rdi
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size relay, .-relay
