#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `f` switches as MSVC compiles a switch for x86-64: the table holds 32-bit
# offsets from the image base, which a lea addresses, the load adds the
# table's offset from the base as its displacement, and the add is encoded
# with the opcode that takes its source from the r/m field. Here `entry`
# stands in for the image base. As for the table of offsets from its own
# start, value analysis cannot bound the index, the table is read from the
# dispatch instructions, and its three entries that lead into `f` are the
# destinations. The fourth entry, which leads into `g`, ends the table. A byte
# before the dispatch that LLVM cannot decode stops the decoding of `f` from its
# start, so the dispatch is decoded back from the jump.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/jump-table-extent.ld %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s < %t.ll
#
# CHECK: "bb.0x401019:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# CHECK-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# CHECK-NEXT: i64 4198464, label %"bb.0x401040:Code_x86_64"
# CHECK-NEXT: i64 4198480, label %"bb.0x401050:Code_x86_64"
# CHECK-NEXT: i64 4198496, label %"bb.0x401060:Code_x86_64"
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
  # shl al, 0 in the /6 encoding protected code uses, which LLVM's decoder
  # rejects: the dispatch is found decoding back from the jump
  .byte 0xc0, 0xf0, 0x00
  lea entry(%rip), %rcx
  mov (table - entry)(%rcx,%rax,4), %edx
  # add %rcx, %rdx with opcode 0x03
  .byte 0x48, 0x03, 0xd1
  jmp *%rdx
  .org 0x40, 0xcc
case0:
  mov $1, %eax
  add $8, %rsp
  ret
  .org 0x50, 0xcc
case1:
  mov $2, %eax
  add $8, %rsp
  ret
  .org 0x60, 0xcc
case2:
  mov $3, %eax
  add $8, %rsp
  ret
  .cfi_endproc
.size f, .-f

.org 0x80
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
