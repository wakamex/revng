#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Under the Microsoft x64 ABI the caller reserves 32 bytes above the return
# address for the four register arguments, and the callee may spill registers
# there, as `spill` does with its two 32-bit arguments. Those stores make a
# stack area that ends inside the reserved part, which holds no arguments: the
# conversion to C found no stack argument at offset 32 and kept `spill` raw.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.c.ptml
# RUN: sed 's/<[^>]*>//g' %t.c.ptml | FileCheck %s
#
# CHECK: _Code_x86_64(generic{{[0-9]+}}_t argument_0, generic{{[0-9]+}}_t argument_1) {

    .intel_syntax noprefix
    .text
    .globl start
start:
    sub rsp, 40
    mov ecx, 2
    mov edx, 3
    call spill
    add rsp, 40
    ret

    .globl spill
spill:
    mov dword ptr [rsp + 8], ecx
    mov dword ptr [rsp + 16], edx
    mov eax, dword ptr [rsp + 8]
    add eax, dword ptr [rsp + 16]
    ret
