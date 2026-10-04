#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `add2` has no prototype until detect-abi, so the first C emission builds it
# with the default prototype. Once detect-abi and the C ABI conversion give it
# one, everything built from the empty prototype must be built again: the
# project that emitted C before then must emit the same C as a fresh project
# that only receives the final model.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: rm -rf %t.a %t.b && mkdir -p %t.a %t.b
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' analyze parse-binary -o /dev/null
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.raw.c.ptml
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' analyze detect-abi -o /dev/null
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' analyze convert-functions-to-cabi -o /dev/null
# RUN: cd %t.a && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.a.c.ptml
# RUN: cd %t.b && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cp %t.a/revng.yml %t.b/revng.yml
# RUN: cd %t.b && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.b.c.ptml
# RUN: diff %t.a.c.ptml %t.b.c.ptml

    .intel_syntax noprefix
    .text
    .globl _start
_start:
    mov edi, 2
    mov esi, 3
    call add2
    mov edi, eax
    mov eax, 60
    syscall

    .globl add2
    .type add2, @function
add2:
    lea eax, [rdi + rsi]
    ret
