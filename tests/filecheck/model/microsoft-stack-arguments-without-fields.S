#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Without data layout analysis the stack arguments of a function are a struct
# with a size and no fields. Under the Microsoft x64 ABI every argument takes
# one 8-byte stack slot after the 32 bytes reserved for the four register
# arguments, so the size alone tells the arguments: `six` reads a quadword in
# the first slot and a byte in the second, a 41-byte area, and takes six
# arguments, the last one byte wide, instead of staying a raw function.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size,convert-functions-to-cabi %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(generic64_t argument_0, generic64_t argument_1, generic64_t argument_2, generic64_t argument_3, generic64_t argument_4, generic8_t argument_5) {

    .intel_syntax noprefix
    .text
    .globl start
start:
    sub rsp, 56
    mov qword ptr [rsp + 32], 5
    mov byte ptr [rsp + 40], 6
    mov ecx, 1
    mov edx, 2
    mov r8d, 3
    mov r9d, 4
    call six
    add rsp, 56
    ret

    .globl six
six:
    lea rax, [rcx + 2 * rdx]
    lea rdx, [r8 + 2 * r8]
    add rax, rdx
    lea rax, [rax + 4 * r9]
    add rax, qword ptr [rsp + 40]
    movzx edx, byte ptr [rsp + 48]
    add rax, rdx
    ret
