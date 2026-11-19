#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A string runs up to the first zero byte, but a string with no terminator, such
# as a Rust one, is followed directly by the next object. Here `unterminated` is
# followed by -inf, whose low bytes are zero, so its reading includes the first
# byte of -inf, which the code loads on its own: the string is its 8 bytes
# before -inf, with no terminator, and the C prints it as a string literal.
# `merged` is used both from its start and from a tail it shares, as linkers
# merge strings that end the same way: it is still the whole string, and so is
# `plain`.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: env REVNG_OPTIONS=-debug-log=detect-c-strings %root/bin/revng quick analyze --analyses=parse-binary,detect-abi,detect-stack-size,detect-c-strings %t > %t.yml 2> %t.log
# RUN: FileCheck %s < %t.log
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck --check-prefix=C %s
#
# CHECK-DAG: Added the first 8 bytes (runs into another object) of "extern \22\00"
# CHECK-DAG: Added "a long merged string\00"
# CHECK-DAG: Added "a plain string\00"
#
# C: "extern \""

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    lea rdi, [rip + unterminated]
    call consume
    movsd xmm0, qword ptr [rip + negative_infinity]
    lea rdi, [rip + merged]
    call consume
    lea rdi, [rip + merged + 7]
    call consume
    lea rdi, [rip + plain]
    call consume
    mov eax, 60
    xor edi, edi
    syscall

consume:
    movzx eax, byte ptr [rdi]
    ret

    .section .rodata
    .p2align 3
unterminated:
    .ascii "extern \""
negative_infinity:
    .quad 0xfff0000000000000
merged:
    .asciz "a long merged string"
plain:
    .asciz "a plain string"
