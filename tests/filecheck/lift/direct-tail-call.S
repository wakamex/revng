#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` jumps to `helper`, and `other` branches to it on a condition, each
# with its stack as on entry: both are tail calls to `helper`. They used to
# copy `helper`'s code instead, so the C of every caller of a function reached
# by a jump repeated its body; on hl-node, such copies were half of the code
# of all functions.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0, generic64_t argument_1) {
# CHECK-NOT: * 3UL
# CHECK: = [[HELPER:function_0x[0-9a-f]+_Code_x86_64]](argument_1);
# CHECK-NOT: * 3UL
# CHECK: {{^}$}}
# CHECK: generic64_t [[HELPER]](generic64_t argument_0) {
# CHECK-NEXT: return argument_0 * 3UL;
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0) {
# CHECK-NOT: * 3UL
# CHECK: = [[HELPER]](argument_0);
# CHECK-NOT: * 3UL
# CHECK: {{^}$}}

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  test %rdi, %rdi
  je 1f
  lea 1(%rdi), %rax
  ret
1:
  mov %rsi, %rdi
  jmp helper
  .cfi_endproc
.size entry, .-entry

.p2align 4, 0xcc
.type helper, @function
helper:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  .cfi_offset %rbx, -16
  lea (%rdi,%rdi,2), %rbx
  mov %rbx, %rax
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size helper, .-helper

.p2align 4, 0xcc
.globl other
.type other, @function
other:
  .cfi_startproc
  cmp $10, %rdi
  ja helper
  lea 2(%rdi), %rax
  ret
  .cfi_endproc
.size other, .-other

.section .data.rel.ro,"aw"
.quad other
