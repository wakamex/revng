#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `helper` starts a function of the unwind information, and `entry` reaches
# it only by a tail call, a direct jump. The start of a function in the unwind
# information is a function entry however code reaches it, so `helper` is a
# function. It used to be one only when a pointer in data named it; reached by
# a jump alone, its code was only part of `entry`. Rust emits such tail calls,
# and hl-node's post-fill check at 0x3ae3bb0 is reached only this way.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -e entry %t.o -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Functions:
# CHECK-NEXT: - Entry: "[[ENTRY:0x[0-9a-f]+]]:Code_x86_64"
# CHECK: - Entry: "{{0x[0-9a-f]+}}:Code_x86_64"
# CHECK: TypeDefinitions:

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
