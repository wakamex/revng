#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# detect-c-strings reads a string at every address the code uses. The bytes
# from the code address it sees run into `printable_code`, whose instructions
# `push rax` to `push rdi` spell "PQRSTUVW" before a zero byte, so they read as
# a long enough UTF-16 string. Those bytes are code the lift decodes, so the
# analysis must not claim them; the string in .rodata, which this static link
# places in the executable segment, is still a string.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: env REVNG_OPTIONS=-debug-log=detect-c-strings %root/bin/revng quick analyze --analyses=parse-binary,detect-abi,detect-stack-size,detect-c-strings %t > %t.yml 2> %t.log
# RUN: FileCheck %s < %t.log
#
# CHECK-DAG: Added "a real string\00"
# CHECK-DAG: Discarded (in code) "{{.*}}PQRSTUVW
# CHECK-NOT: Added "{{.*}}PQRSTUVW

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    lea rdi, [rip + real_string]
    call consume
    lea rdi, [rip + printable_code]
    call consume
    call printable_code
    mov eax, 60
    xor edi, edi
    syscall

consume:
    movzx eax, byte ptr [rdi]
    ret

printable_code:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
    push rbp
    push rsi
    push rdi
    add byte ptr [rax], al
    add rsp, 64
    ret

    .section .rodata
real_string:
    .asciz "a real string"
