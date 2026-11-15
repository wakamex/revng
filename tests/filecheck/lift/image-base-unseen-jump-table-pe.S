#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# In a PE program, a switch MSVC compiles loads a 32-bit offset through a
# register, adds that register and jumps: the register holds the image base,
# and the table lies at the image base plus the load's displacement, even when
# the lea that loads the base cannot be decoded back from the jump. Here the lea
# lies at `f`'s start, far before a byte LLVM cannot decode, which stops the
# decoding of `f` from its start, so the dispatch is decoded back from the jump
# only a few bytes. The table's three entries that lead into `f` are the
# destinations.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t.exe -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s < %t.ll
#
# CHECK: switch i64 %{{[0-9]+}}, label %unexpectedpc [
# CHECK-NEXT: i64 5368713472, label %"bb.0x140001100:Code_x86_64"
# CHECK-NEXT: i64 5368713488, label %"bb.0x140001110:Code_x86_64"
# CHECK-NEXT: i64 5368713504, label %"bb.0x140001120:Code_x86_64"
# CHECK-NEXT: ]

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  call f
  add $0x28, %rsp
  ret
.seh_endproc

.p2align 6
.def f; .scl 3; .type 32; .endef
.seh_proc f
f:
  push %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  lea __ImageBase(%rip), %rbx
  .fill 48, 1, 0x90
  # shl al, 0 in the /6 encoding protected code uses, which LLVM's decoder
  # rejects
  .byte 0xc0, 0xf0, 0x00
  mov table@IMGREL(%rbx,%rcx,4), %eax
  add %rbx, %rax
  jmp *%rax
  .p2align 8, 0xcc
case0:
  mov $1, %eax
  pop %rbx
  ret
  .p2align 4, 0xcc
case1:
  mov $2, %eax
  pop %rbx
  ret
  .p2align 4, 0xcc
case2:
  mov $3, %eax
  pop %rbx
  ret
.seh_endproc

.section .rdata, "dr"
.p2align 2
table:
  .rva case0
  .rva case1
  .rva case2
  .rva start
