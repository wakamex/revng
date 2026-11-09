#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `_start` and `other` each end in an `exit` system call, which nothing tells
# rev.ng never returns, so each runs on into the next function and holds a copy
# of its code. Both next functions branch back to their own entry: `halve`
# loops there and `spin` jumps to itself. A branch to another function's entry
# is a tail call, and the outliner made one of the branch in the copy, whose
# target the copy holds too, leaving the call in a block nothing reached.
# detect-abi stopped at an assertion in `createAnyPCHooks` on `_start`, and
# function isolation crashed in `handleCall` on a function running on into one
# like `spin`, which never returns. A branch to an entry the function has its
# own copy of stays a branch into that copy.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -s --eh-frame-hdr -e _start %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0, generic64_t argument_1, generic64_t argument_2) {
# CHECK-DAG: >> 1UL
# CHECK-DAG: = [[HALVE:function_0x[0-9a-f]+_Code_x86_64]](argument_0);
# CHECK: generic64_t [[HALVE]](generic64_t argument_0) {
# CHECK: >> 1UL
# CHECK: return 7UL;
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0) {
# CHECK: [[SPIN:function_0x[0-9a-f]+_Code_x86_64]]();
# CHECK: void [[SPIN]](void) {
# CHECK-NEXT: while (true) {

    .text
    .globl _start
    .type _start, @function
_start:
    .cfi_startproc
    test %rsi, %rsi
    je 1f
    call halve
    ret
1:
    test %rdx, %rdx
    je 2f
    call other
2:
    mov $60, %eax
    syscall
    .cfi_endproc

    .type halve, @function
halve:
    .cfi_startproc
    shr %rdi
    jne halve
    mov $7, %eax
    ret
    .cfi_endproc

    .type other, @function
other:
    .cfi_startproc
    test %rdi, %rdi
    je 1f
    call spin
1:
    mov $60, %eax
    syscall
    .cfi_endproc

    .type spin, @function
spin:
    .cfi_startproc
    jmp spin
    .cfi_endproc
