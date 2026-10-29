#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` dispatches on the byte `kind` returns, through a table of 32-bit
# offsets from the table, as compilers do for an enum discriminant. The index
# is not bounded, so the switch cannot be rewritten over it, and its value is
# the table's address plus the entry read from it. The case values are code
# addresses, which match that value only where the table's segment sits at its
# original address. The C switches on the entry instead, the target's offset
# from the table, with the cases' offsets as case values.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: switch (
# CHECK-NOT: &segment_0 + 525UL +
# CHECK-SAME: {
# CHECK-NEXT: case 4227U:
# CHECK: case 4233U:
# CHECK: case 4239U:

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  mov %rsi, %rbx
  call kind
  movzbl %al, %eax
  lea table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  add %rcx, %rax
  jmp *%rax
case0:
  lea 1(%rbx), %rax
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
case1:
  .cfi_def_cfa_offset 16
  lea (%rbx,%rbx), %rax
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
case2:
  .cfi_def_cfa_offset 16
  mov %rbx, %rax
  neg %rax
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

.type kind, @function
kind:
  .cfi_startproc
  mov %edi, %eax
  ret
  .cfi_endproc
.size kind, .-kind

.section .rodata
table:
  .long case0 - table
  .long case1 - table
  .long case2 - table
  .long kind - table
