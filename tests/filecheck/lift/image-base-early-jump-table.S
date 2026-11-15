#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `f` switches as MSVC compiles a switch for x86-64, with the image base
# loaded into a register once, early in the function, as MSVC does when the
# function uses it more than once: the lea lies far before the dispatch, and
# the register the dispatch jumps through is written in between, before the
# table is loaded into it. Here `entry` stands in for the image base. The
# table's three entries that lead into `f` are the destinations; the fourth,
# which leads into `g`, ends the table.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/jump-table-extent.ld %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s < %t.ll
#
# CHECK: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# CHECK-NEXT: i64 4198496, label %"bb.0x401060:Code_x86_64"
# CHECK-NEXT: i64 4198512, label %"bb.0x401070:Code_x86_64"
# CHECK-NEXT: i64 4198528, label %"bb.0x401080:Code_x86_64"
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
  push %rbx
  .cfi_def_cfa_offset 16
  lea entry(%rip), %rbx
  call g
  mov (%rsp), %eax
  mov $7, %edx
  add %edx, %ecx
  xor %r8d, %r8d
  inc %r8d
  add %r8d, %ecx
  mov %ecx, %r9d
  sub %r9d, %ecx
  mov (table - entry)(%rbx,%rax,4), %edx
  add %rbx, %rdx
  jmp *%rdx
  .org 0x60, 0xcc
case0:
  mov $1, %eax
  pop %rbx
  ret
  .org 0x70, 0xcc
case1:
  mov $2, %eax
  pop %rbx
  ret
  .org 0x80, 0xcc
case2:
  mov $3, %eax
  pop %rbx
  ret
  .cfi_endproc
.size f, .-f

.org 0xa0
.type g, @function
g:
  .cfi_startproc
  movl $1, -8(%rsp)
  ret
  .cfi_endproc
.size g, .-g

.p2align 4, 0xcc
table:
  .long case0 - entry
  .long case1 - entry
  .long case2 - entry
  .long g - entry
