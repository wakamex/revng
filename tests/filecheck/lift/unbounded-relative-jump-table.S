#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `f` switches on a 32-bit value it loads after a call, without checking its
# range, as compilers do for an enum discriminant: the dispatch reads a table
# of 32-bit offsets from the table's start. Value analysis cannot bound the
# index, so it gives no destinations; the table is read from the dispatch
# instructions instead, and its three entries that lead into `f` are the
# destinations. The fourth entry, which leads into `g`, ends the table.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/jump-table-extent.ld %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s < %t.ll
#
# CHECK: "bb.0x401019:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# CHECK-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# CHECK-NEXT: i64 4198444, label %"bb.0x40102c:Code_x86_64"
# CHECK-NEXT: i64 4198464, label %"bb.0x401040:Code_x86_64"
# CHECK-NEXT: i64 4198480, label %"bb.0x401050:Code_x86_64"
# CHECK-NEXT: ]

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call f
  ret
  .cfi_endproc
.size entry, .-entry

.org 0x10
.type f, @function
f:
  .cfi_startproc
  sub $8, %rsp
  .cfi_def_cfa_offset 16
  call g
  mov (%rsp), %eax
  lea table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  add %rcx, %rax
  jmp *%rax
case0:
  mov $1, %eax
  add $8, %rsp
  ret
  .org 0x40, 0xcc
case1:
  mov $2, %eax
  add $8, %rsp
  ret
  .org 0x50, 0xcc
case2:
  mov $3, %eax
  add $8, %rsp
  ret
  .cfi_endproc
.size f, .-f

.org 0x70
.type g, @function
g:
  .cfi_startproc
  movl $1, -8(%rsp)
  ret
  .cfi_endproc
.size g, .-g

.section .rodata
table:
  .long case0 - table
  .long case1 - table
  .long case2 - table
  .long g - table
