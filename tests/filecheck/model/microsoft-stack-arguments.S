#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Under the Microsoft x64 ABI the first stack argument sits after the 32 bytes
# the caller reserves for the four register arguments. `five` takes four
# arguments in registers and a fifth on the stack. Data layout analysis types
# its stack arguments as a union of two structs of the whole area, one for what
# `start` writes and one for what `five` reads, both with an 8-byte field at
# offset 32. The conversion to a C prototype must look through the union and
# expect the first stack argument at offset 32, so `five` gets five arguments
# instead of staying a raw function.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: function_0x14000102d_Code_x86_64(generic64_t argument_0, generic64_t argument_1, generic64_t argument_2, generic64_t argument_3, {{[a-z_0-9]+}} argument_4) {

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
    lea rax, [rcx + 2 * rdx]
    lea rdx, [r8 + 2 * r8]
    add rax, rdx
    lea rax, [rax + 4 * r9]
    mov rdx, qword ptr [rsp + 40]
    lea rdx, [rdx + 4 * rdx]
    add rax, rdx
    ret
