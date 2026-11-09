#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# As in writable-slot-callees.S, `wrapper` calls through `slot`, which holds
# `resolver`, `fast` or `slow`, and passes xmm0 to xmm2 on without touching
# them; here both `fast` and `slow` compute in xmm0. The call reads what the
# functions the slot holds consume, as a call to one of them does, so the
# three doubles of `entry` reach them. Only the registers `wrapper` mentions
# itself were followed through the call, gaining one at each round of the
# analysis: `wrapper` took xmm0 and xmm1 alone and passed zero in xmm2.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -z relro -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: float64_t var_{{[0-9]+}} = function_0x{{[0-9a-f]+}}_Code_x86_64(var_{{[0-9]+}}, var_{{[0-9]+}}, bit_cast(float64_t, int64_to_float64(argument_2
# CHECK: float64_t function_0x{{[0-9a-f]+}}_Code_x86_64(float64_t argument_0, float64_t argument_1, float64_t argument_2) {
# CHECK-NEXT: float64_t var_0 = ((cabifunction_{{[0-9]+}} *) {{.+}})(argument_0, argument_1, argument_2);

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
  mov slot(%rip), %rax
  call *%rax
  pop %rbp
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size wrapper, .-wrapper

.type resolver, @function
resolver:
  .cfi_startproc
  mov feature(%rip), %eax
  lea fast(%rip), %rcx
  lea slow(%rip), %rdx
  test %eax, %eax
  cmovne %rcx, %rdx
  mov %rdx, slot(%rip)
  jmp *%rdx
  .cfi_endproc
.size resolver, .-resolver

.type fast, @function
fast:
  .cfi_startproc
  mulsd %xmm1, %xmm0
  addsd %xmm2, %xmm0
  ret
  .cfi_endproc
.size fast, .-fast

.type slow, @function
slow:
  .cfi_startproc
  mulsd %xmm1, %xmm0
  addsd %xmm2, %xmm0
  ret
  .cfi_endproc
.size slow, .-slow

.data
slot:
  .quad resolver
feature:
  .long 1
