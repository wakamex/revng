#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -function-pipe-jobs, collect-cfg runs its optimization pipeline on
# worker threads, and the per-function pass pipes move each function to a
# worker through bitcode. The C emitted with one worker and with four is the
# same, and on this program it is also the same as without the option.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: rm -rf %t.none %t.one %t.four && mkdir -p %t.none %t.one %t.four
# RUN: cd %t.none && %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size %t -o %t.none.c.ptml > %t.none.log 2>&1
# RUN: cd %t.one && env REVNG_OPTIONS=-function-pipe-jobs=1 %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size %t -o %t.one.c.ptml > %t.one.log 2>&1
# RUN: cd %t.four && env REVNG_OPTIONS=-function-pipe-jobs=4 %root/bin/revng quick artifact emit-c-as-single-file --analyses=parse-binary,detect-abi,detect-stack-size %t -o %t.four.c.ptml > %t.four.log 2>&1
# RUN: diff %t.one.c.ptml %t.four.c.ptml
# RUN: diff %t.none.c.ptml %t.one.c.ptml

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
