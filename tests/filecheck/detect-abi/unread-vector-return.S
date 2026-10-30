#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `scale` computes its result in `xmm0` and returns it in `rax`, so its last
# write to `xmm0` reaches the return without being read. `entry`, its only
# caller, reads `rax` alone after the call. A vector register a function
# writes and does not read before returning is a return value only if a
# caller may read it, so `scale` returns `rax` alone and keeps a C prototype;
# it used to be raw, returning `rax` and `zmm0` in a structure, which C
# returns through memory.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: zmm0
# CHECK: return function_0x{{[0-9a-f]+}}_Code_x86_64(argument_0) + 1
# CHECK: _ABI(SystemV_x86_64)
# CHECK-NEXT: generic64_t function_0x{{[0-9a-f]+}}_Code_x86_64(generic64_t argument_0) {
# CHECK-NOT: zmm0

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call scale
  add $1, %rax
  ret
  .cfi_endproc
.size entry, .-entry

.type scale, @function
scale:
  .cfi_startproc
  movd %edi, %xmm0
  punpcklbw %xmm0, %xmm0
  movq %xmm0, %rax
  ret
  .cfi_endproc
.size scale, .-scale
