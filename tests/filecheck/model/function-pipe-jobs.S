#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -function-pipe-jobs, function pipes that can (collect-cfg) run their
# optimization pipelines on worker threads, and their results and recorded
# model reads are those of one thread: the model after detect-stack-size and
# the emitted C are the same as with one thread.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: rm -rf %t.one %t.four && mkdir -p %t.one %t.four
# RUN: cd %t.one && %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size %t -o %t.one.c.ptml > %t.one.log 2>&1
# RUN: cd %t.four && env REVNG_OPTIONS=-function-pipe-jobs=4 %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size %t -o %t.four.c.ptml > %t.four.log 2>&1
# RUN: diff %t.one.c.ptml %t.four.c.ptml

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    mov edi, 2
    mov esi, 3
    call add2
    mov edi, eax
    call twice
    mov edi, eax
    call pick
    mov edi, eax
    mov eax, 60
    syscall

    .globl add2
    .type add2, @function
add2:
    lea eax, [rdi + rsi]
    ret

    .globl twice
    .type twice, @function
twice:
    push rbx
    mov ebx, edi
    mov esi, edi
    call add2
    pop rbx
    ret

    .globl pick
    .type pick, @function
pick:
    cmp edi, 4
    ja 1f
    lea eax, [rdi + rdi * 2]
    ret
1:
    mov eax, -1
    ret
