#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Under the Microsoft x64 ABI the argument at position I is in the I-th general
# purpose or vector register. `scale` takes two doubles in xmm0 and xmm1 and an
# integer in r8, and returns a double in xmm0. Its C prototype has to say so:
# the values in vector registers are doubles, since a generic value would be
# passed in a general purpose register, and the arguments are in the order of
# their positions.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: float64_t function_0x[[#%x,SCALE:]]_Code_x86_64(float64_t argument_0, float64_t argument_1, generic{{[0-9]+}}_t argument_2) {

    .intel_syntax noprefix
    .text
    .globl start
start:
    sub rsp, 40
    movsd xmm0, qword ptr [rip + one]
    movsd xmm1, qword ptr [rip + two]
    mov r8d, 3
    call scale
    cvttsd2si eax, xmm0
    add rsp, 40
    ret

    .globl scale
scale:
    test r8d, r8d
    je 2f
1:
    mulsd xmm0, xmm1
    dec r8d
    jne 1b
2:
    ret

    .data
one:
    .double 1.5
two:
    .double 2.0
