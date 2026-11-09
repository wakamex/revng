#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `first` ends in a call through a pointer that never returns, which nothing
# tells rev.ng, and the padding after it leads to `second`. The code of `first`
# was taken to run on into `second` as a tail call, so `first` returned what
# `second` returns, at a stack offset that is not its own, and its real return
# was a broken one. Callers analyzed before such a function is corrected read
# the wrong stack offset and are marked as never returning, which made the
# model of a program depend on the order its functions are analyzed in.
#
# The unwind information has `second` start where the padding ends, so no code
# of `first` follows the call: the call does not return.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -s --eh-frame-hdr -e _start %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0) {
# CHECK-NOT: function_0x
# CHECK-NOT: revng_abort
# CHECK: return argument_0 + 1UL;
# CHECK-NEXT: {{^}$}}
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}(generic64_t argument_0) {
# CHECK-NEXT: return argument_0 * 3UL;

    .text
    .globl _start
    .type _start, @function
_start:
    .cfi_startproc
    call first
    mov %rax, %rdi
    call second
    mov %eax, %edi
    mov $60, %eax
    syscall
    ud2
    .cfi_endproc

    .p2align 4
    .type first, @function
first:
    .cfi_startproc
    test %rdi, %rdi
    je 1f
    lea 1(%rdi), %rax
    ret
1:
    push %rax
    .cfi_def_cfa_offset 16
    call *handler(%rip)
    .cfi_endproc

    .p2align 4
    .type second, @function
second:
    .cfi_startproc
    lea (%rdi,%rdi,2), %rax
    ret
    .cfi_endproc

    .data
handler:
    .quad 0
