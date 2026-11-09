#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `wrapper` returns what `soft` computes in xmm0, and `entry` reads xmm0 after
# calling `wrapper`. A vector register is a return value of a function only if
# a caller reads it after a call to it; `wrapper` reads xmm0 after calling
# `soft` by returning it, so `soft` returns zmm0. After a call the analysis
# writes the return values of the callee, and it used to look for what the
# caller reads before those writes, where a register the callee returns is
# never alive: `soft` returned nothing, and `wrapper` returned its own
# argument.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -z relro -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: float64_t function_0x{{[0-9a-f]+}}_Code_x86_64(float64_t argument_0, float64_t argument_1, float64_t argument_2) {
# CHECK-NEXT: return function_0x{{[0-9a-f]+}}_Code_x86_64(argument_0, argument_1, argument_2);
# CHECK: float64_t function_0x{{[0-9a-f]+}}_Code_x86_64(float64_t argument_0, float64_t argument_1, float64_t argument_2) {
# CHECK-NEXT: float64_add(float64_mul(bit_cast(generic64_t, argument_0), bit_cast(generic64_t, argument_1),

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  sub $8, %rsp
  .cfi_def_cfa_offset 16
  cvtsi2sd %rdi, %xmm0
  cvtsi2sd %rsi, %xmm1
  cvtsi2sd %rdx, %xmm2
  call wrapper
  cvttsd2si %xmm0, %rax
  add $8, %rsp
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

.type wrapper, @function
wrapper:
  .cfi_startproc
  push %rbp
  .cfi_def_cfa_offset 16
  mov %rsp, %rbp
  call soft
  pop %rbp
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size wrapper, .-wrapper

.type soft, @function
soft:
  .cfi_startproc
  mulsd %xmm1, %xmm0
  addsd %xmm2, %xmm0
  ret
  .cfi_endproc
.size soft, .-soft
