#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `scale` converts its integer argument to a double, moves it to xmm0 and
# calls `halve`, whose prototype, from the debug information, takes a double.
# The move writes the low 16 bytes of zmm0 and the double is in the low 8. A
# call to a function with a prototype was taken to read the whole register of
# each argument, so the other 48 bytes of zmm0 seemed to reach `halve` from
# the caller of `scale`: `scale` took all of zmm0 as an argument, and so did
# every function calling it. `scale` takes its integer alone.
#
# RUN: printf '__attribute__((noinline)) double halve(double x) { return x / 2; }\n' > %t.c
# RUN: clang --target=x86_64-unknown-linux-gnu -g -O1 -c %t.c -o %t.dep.o
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -e scale %t.o %t.dep.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: scale(generic64_t argument_0) {
# CHECK: halve(

    .text
    .globl scale
    .type scale, @function
scale:
    sub $8, %rsp
    pxor %xmm1, %xmm1
    cvtsi2sd %rdi, %xmm1
    movapd %xmm1, %xmm0
    call halve
    cvttsd2si %xmm0, %rax
    add $8, %rsp
    ret
