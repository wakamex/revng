#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Jump tables and the functions they belong to, as the unwind information
# describes them.
#
# `f` switches on a byte without checking its range, as compilers do when they
# know all values are handled; value materialization reads all 256 entries,
# and the 252 after the real table point into `g`. Only the four entries that
# point into `f` are destinations.
#
# `c` checks its index first, so its four destinations come out as before.
#
# `h` is split into a hot part and a cold part with an unwind entry of its own,
# which starts with the frame of `h` set up, and the table of `h` points into
# both; data pointing into `g` follows it. The destination in the cold part is
# kept, and so is the entry after it.
#
# `m` has two switches whose tables are adjacent: `a_table`, indexed by an
# unchecked byte, and then `b_table`. Read relative to `a_table`, the entries
# of `b_table` point 8 bytes before the cases of the second switch, which are
# instructions of `m`; the table of the first switch still ends where the
# code of `m` refers to `b_table`. Taking those as destinations would also
# split the block of the second switch's jump and leave it unresolved.
#
# `t` jumps through a table of functions, `g` and `k`: none of the
# destinations is in `t`, so the table is not one of its own and every
# destination is kept.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/jump-table-extent.ld %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=F < %t.ll
# RUN: FileCheck %s --check-prefix=C < %t.ll
# RUN: FileCheck %s --check-prefix=H < %t.ll
# RUN: FileCheck %s --check-prefix=T < %t.ll
# RUN: FileCheck %s --check-prefix=M1 < %t.ll
# RUN: FileCheck %s --check-prefix=M2 < %t.ll
#
# Each jump through a table ends in a switch on the destination's address, in
# the block of the jump, after the checks on the address's epoch,
# address space and type. 0x4010c0 is `g_mid`, in `g`.
#
# F: "bb.0x40101f:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# F-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# F-NEXT: i64 4198451, label %"bb.0x401033:Code_x86_64"
# F-NEXT: i64 4198457, label %"bb.0x401039:Code_x86_64"
# F-NEXT: i64 4198463, label %"bb.0x40103f:Code_x86_64"
# F-NEXT: i64 4198469, label %"bb.0x401045:Code_x86_64"
# F-NEXT: ]
#
# C: "bb.0x401052:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# C-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# C-NEXT: i64 4198500, label %"bb.0x401064:Code_x86_64"
# C-NEXT: i64 4198506, label %"bb.0x40106a:Code_x86_64"
# C-NEXT: i64 4198512, label %"bb.0x401070:Code_x86_64"
# C-NEXT: i64 4198518, label %"bb.0x401076:Code_x86_64"
# C-NEXT: ]
#
# H: "bb.0x40107f:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# H-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# H-NEXT: i64 4198550, label %"bb.0x401096:Code_x86_64"
# H-NEXT: i64 4198557, label %"bb.0x40109d:Code_x86_64"
# H-NEXT: i64 4198564, label %"bb.0x4010a4:Code_x86_64"
# H-NEXT: i64 4198602, label %"bb.0x4010ca:Code_x86_64"
# H-NEXT: ]
#
# T: "bb.0x4010b3:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# T-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# T-NEXT: i64 4198587, label %"bb.0x4010bb:Code_x86_64"
# T-NEXT: i64 4198596, label %"bb.0x4010c4:Code_x86_64"
# T-NEXT: ]
#
# M1: "bb.0x4010d1:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# M1-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# M1-NEXT: i64 4198629, label %"bb.0x4010e5:Code_x86_64"
# M1-NEXT: i64 4198635, label %"bb.0x4010eb:Code_x86_64"
# M1-NEXT: ]
# M2: "bb.0x4010f0:Code_x86_64_epoch_0_address_space_0_type_Code_x86_64":
# M2-NEXT: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# M2-NEXT: i64 4198664, label %"bb.0x401108:Code_x86_64"
# M2-NEXT: i64 4198670, label %"bb.0x40110e:Code_x86_64"
# M2-NEXT: ]

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call f
  call c
  call h
  call t
  call g
  call k
  ret
  .cfi_endproc
.size entry, .-entry

.globl f
.type f, @function
f:
  .cfi_startproc
  movzbl %dil, %eax
  leaq f_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
f0:
  movl $10, %eax
  ret
f1:
  movl $11, %eax
  ret
f2:
  movl $12, %eax
  ret
f3:
  movl $13, %eax
  ret
  .cfi_endproc
.size f, .-f
  int3
  int3

.globl c
.type c, @function
c:
  .cfi_startproc
  cmpl $3, %edi
  ja c_default
  movl %edi, %eax
  leaq c_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
c0:
  movl $20, %eax
  ret
c1:
  movl $21, %eax
  ret
c2:
  movl $22, %eax
  ret
c3:
  movl $23, %eax
  ret
c_default:
  xorl %eax, %eax
  ret
  .cfi_endproc
.size c, .-c

.globl h
.type h, @function
h:
  .cfi_startproc
  pushq %rbx
  .cfi_def_cfa_offset 16
  .cfi_offset %rbx, -16
  movl %esi, %ebx
  movzbl %dil, %eax
  leaq h_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
h0:
  movl $30, %eax
  jmp h_exit
h1:
  movl $31, %eax
  jmp h_exit
h3:
  movl $33, %eax
h_exit:
  addl %ebx, %eax
  popq %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size h, .-h

.globl t
.type t, @function
t:
  .cfi_startproc
  cmpq $1, %rdi
  ja t_out
  jmpq *t_table(,%rdi,8)
t_out:
  ret
  .cfi_endproc
.size t, .-t

.globl g
.type g, @function
g:
  .cfi_startproc
  movl $40, %eax
g_mid:
  addl $1, %eax
  ret
  .cfi_endproc
.size g, .-g

.globl k
.type k, @function
k:
  .cfi_startproc
  movl $50, %eax
  ret
  .cfi_endproc
.size k, .-k

# The cold part of `h`, placed away from it as compilers do
.type h_cold, @function
h_cold:
  .cfi_startproc
  .cfi_def_cfa_offset 16
  .cfi_offset %rbx, -16
  movl $32, %eax
  jmp h_exit
  .cfi_endproc
.size h_cold, .-h_cold

.globl m
.type m, @function
m:
  .cfi_startproc
  movzbl %dil, %eax
  leaq a_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
a0:
  movl $60, %eax
  ret
a1:
  cmpl $1, %esi
  ja b_default
  movl %esi, %eax
  leaq b_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  # 8 bytes up to b0, the first case of the second switch
  addq $1, %rdx
  nop
  nop
  jmpq *%rax
b0:
  movl $70, %eax
  ret
b1:
  movl $71, %eax
  ret
b_default:
  xorl %eax, %eax
  ret
  .cfi_endproc
.size m, .-m

.section .rodata
.p2align 3
t_table:
  .quad g
  .quad k
f_table:
  .long f0 - f_table
  .long f1 - f_table
  .long f2 - f_table
  .long f3 - f_table
  .rept 252
  .long g_mid - f_table
  .endr
c_table:
  .long c0 - c_table
  .long c1 - c_table
  .long c2 - c_table
  .long c3 - c_table
  .rept 4
  .long g_mid - c_table
  .endr
h_table:
  .long h0 - h_table
  .long h1 - h_table
  .long h_cold - h_table
  .long h3 - h_table
  .rept 252
  .long g_mid - h_table
  .endr
a_table:
  .long a0 - a_table
  .long a1 - a_table
b_table:
  .long b0 - b_table
  .long b1 - b_table
  .rept 252
  .long g_mid - b_table
  .endr

.section .note.GNU-stack,"",@progbits
