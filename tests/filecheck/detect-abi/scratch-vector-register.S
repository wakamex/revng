#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` copies 8 bytes through `xmm0`: `movq` zeroes the rest of the
# register, which nothing reads. It then calls `leaf`, which reads only `rdi`.
# A register written before a call and not read afterwards is an argument of
# the call, but it becomes an argument of the callee only if the callee reads
# it or passes it on unchanged to a call through a pointer. `leaf` takes `rdi`
# alone and keeps a C prototype; it used to take `zmm0` too, which made it a
# raw function with a 512-bit parameter.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: zmm0
# CHECK: return function_0x{{[0-9a-f]+}}_Code_x86_64(argument_0);
# CHECK-NOT: zmm0
# CHECK: _ABI(SystemV_x86_64)
# CHECK-NEXT: generic64_t function_0x{{[0-9a-f]+}}_Code_x86_64({{[a-z_0-9]+}} *argument_0) {
# CHECK-NEXT: return argument_0->
# CHECK-NOT: zmm0

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  movq (%rsi), %xmm0
  movq %xmm0, (%rdi)
  call leaf
  ret
  .cfi_endproc
.size entry, .-entry

.type leaf, @function
leaf:
  .cfi_startproc
  mov (%rdi), %rax
  ret
  .cfi_endproc
.size leaf, .-leaf
