#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `relay` ends with a tail call through a pointer from its object, as Rust's
# Formatter::pad_integral ends calling the output's `write_str` through its
# vtable. The C calls the pointer and returns its result; it used to return
# the pointer itself, so the call was lost.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(struct_{{[0-9]+}} *argument_0, generic64_t argument_1) {
# CHECK: argument_0->offset_8.member_0)(

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  call relay
  add $1, %rax
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

.type relay, @function
relay:
  .cfi_startproc
  test %rsi, %rsi
  je 1f
  mov 8(%rdi), %rax
  mov (%rdi), %rdi
  jmp *%rax
1:
  xor %eax, %eax
  ret
  .cfi_endproc
.size relay, .-relay
