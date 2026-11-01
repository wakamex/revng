#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` calls through a pointer and then passes `rsi`, which the call does
# not return a value in under the ABI, to a second call through a pointer.
# Nothing but the ABI tells what an unknown callee returns, so the first call
# returns nothing: `rsi` holds what it held before the call. The first call
# used to return `rsi`, which made its prototype raw; in a Rust trait object
# loop, two calls through vtables each kept the other's `rsi`, and the
# recovered C crashed.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: void function_0x{{[0-9a-f]+}}_Code_x86_64({{[a-z_0-9]+}} *argument_0, generic64_t argument_1) {
# CHECK-NEXT: ((cabifunction_{{[0-9]+}} *) argument_0->{{[a-z_0-9.]+}})((generic64_t) argument_0, argument_1);
# CHECK-NEXT: ((cabifunction_{{[0-9]+}} *) argument_0->{{[a-z_0-9.]+}})(argument_1);
# CHECK-NOT: rawfunction

.text
.globl entry
.type entry, @function
entry:
  push %rbx
  mov %rdi, %rbx
  call *(%rbx)
  mov %rsi, %rdi
  call *8(%rbx)
  pop %rbx
  ret
.size entry, .-entry
