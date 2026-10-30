#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `method` is reached only through the pointer in `table`, so no call in the
# binary shows which registers its callers pass. It calls a function pointer
# from its object, so the registers that call receives unchanged from
# `method`'s caller may be arguments `method` forwards, but only the general
# purpose ones: `method` uses `xmm0` only after the call, and a vector register
# holds an argument only of floating point or vector type. It used to take
# `zmm0` too.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared -s --build-id=none -z relro --no-relax %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(
# CHECK-NOT: register_zmm0 _REG
# CHECK-SAME: ) {

.text
.type method, @function
method:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  mov 8(%rdi), %rax
  mov (%rdi), %rdi
  call *%rax
  movq %rax, %xmm0
  movq %xmm0, %rax
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size method, .-method

.section .data.rel.ro,"aw"
.globl table
.type table, @object
table:
  .quad method
.size table, 8
