#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Function modules name the model's types by handle in their bytecode, and
# a container defines those types, with their names, from the model when it
# is used. A helper header and tagless C built from cached function modules,
# read back by a later command, equal what a fresh project builds in a single
# command. `pair` returns two registers, so the C names a model struct.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: rm -rf %t.cached %t.fresh && mkdir -p %t.cached %t.fresh
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' analyze parse-binary -o /dev/null
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' analyze detect-abi -o /dev/null
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.cached.c.ptml
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-helper-header -o %t.cached.h.ptml
# RUN: cd %t.cached && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-tagless-c-as-single-file -o %t.cached.tagless.c
# RUN: cd %t.fresh && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cp %t.cached/revng.yml %t.fresh/revng.yml
# RUN: cd %t.fresh && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-helper-header -o %t.fresh.h.ptml
# RUN: diff %t.cached.h.ptml %t.fresh.h.ptml
# RUN: rm -rf %t.fresh2 && mkdir -p %t.fresh2
# RUN: cd %t.fresh2 && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cp %t.cached/revng.yml %t.fresh2/revng.yml
# RUN: cd %t.fresh2 && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-tagless-c-as-single-file -o %t.fresh.tagless.c
# RUN: diff %t.cached.tagless.c %t.fresh.tagless.c
# RUN: grep -q artificial_struct %t.cached.tagless.c

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
    mov esi, 7
    call pair
    add eax, edx
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

    # Returns in two registers, so detect-abi gives it a raw prototype whose
    # return type is a model struct its caller names
    .globl pair
    .type pair, @function
pair:
    mov eax, edi
    mov edx, esi
    ret
