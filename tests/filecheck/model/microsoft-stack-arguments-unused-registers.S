#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Under the Microsoft x64 ABI arguments go by position: the fifth is on the
# stack after the four register positions, whether the function reads them or
# not. `five` reads only rcx and its fifth argument, so its second to fourth
# positions are unused registers, and it takes five arguments instead of
# staying a raw function.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size,convert-functions-to-cabi %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(generic64_t argument_0, generic64_t argument_1, generic64_t argument_2, generic64_t argument_3, generic64_t argument_4) {

    .intel_syntax noprefix
    .text
    .globl start
start:
    sub rsp, 56
    mov qword ptr [rsp + 32], 5
    mov ecx, 1
    mov edx, 2
    mov r8d, 3
    mov r9d, 4
    call five
    add rsp, 56
    ret

    .globl five
five:
    mov rax, qword ptr [rsp + 40]
    add rax, rcx
    ret
