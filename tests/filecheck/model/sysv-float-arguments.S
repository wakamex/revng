#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `mix` takes a double in xmm0 and an integer in rdi and returns a double in
# xmm0. A value in a vector register is passed only if its C type is a floating
# point one, and rev.ng tracks vector registers in 8-byte parts, so the C
# prototype takes and returns `float64_t`, not the whole of zmm0.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -e _start %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: float64_t mix(generic{{[0-9]+}}_t argument_0, float64_t argument_1) {

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    movsd xmm0, qword ptr [rip + one]
    mov edi, 3
    call mix
    cvttsd2si edi, xmm0
    mov eax, 60
    syscall

    .globl mix
    .type mix, @function
mix:
    cvtsi2sd xmm1, rdi
    mulsd xmm0, xmm1
    ret

    .data
one:
    .double 1.5
