#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `after` is reached only through `table` and starts right after `fail`, whose
# last instruction calls `stop`, which traps and never returns. The address after a call
# is a return address, and the collection of functions from addresses in data
# ignored it even though the unwind information has a function starting there,
# so `after` was no function. A function starting in the unwind information
# means the call before it does not return, and the address becomes a function
# like any other unwind entry pointed to from data.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -s --eh-frame-hdr -e _start %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t function_0x{{[0-9a-f]+}}_Code_x86_64(void) {
# CHECK-NEXT: return 7

    .text
    .globl _start
    .type _start, @function
_start:
    .cfi_startproc
    call *table(%rip)
    mov %eax, %edi
    mov $60, %eax
    syscall
    .cfi_endproc

    .type stop, @function
stop:
    .cfi_startproc
    ud2
    .cfi_endproc

    .type fail, @function
fail:
    .cfi_startproc
    push %rax
    .cfi_def_cfa_offset 16
    call stop
    .cfi_endproc

    .type after, @function
after:
    .cfi_startproc
    mov $7, %eax
    ret
    .cfi_endproc

    .data
table:
    .quad after
    .quad fail
