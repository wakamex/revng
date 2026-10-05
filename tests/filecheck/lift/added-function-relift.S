#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The lift records its jump targets, and a model change adding a function is
# checked against them: a new function at an address that is not a jump
# target, here inside add2, needs the binary lifted again, while lifting again
# without a model change reuses the cached lift.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static %s -o %t
# RUN: printf '0x%%x:Code_x86_64' $((0x$(nm %t | grep ' add2$' | cut -d' ' -f1) + 3)) > %t.inside
# RUN: printf '0x%%x:Code_x86_64' $((0x$(nm %t | grep ' twice$' | cut -d' ' -f1))) > %t.next
# RUN: rm -rf %t.p && mkdir -p %t.p
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' analyze parse-binary -o /dev/null
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' artifact lift -o %t.first.bc
# RUN: cd %t.p && env REVNG_OPTIONS=--trace=%t.cached.json %root/bin/revng project --storage-provider 'local://?inline' artifact lift -o %t.cached.bc
# RUN: not grep -q '"name": "Running lift", "cat": "task", "ph": "B"' %t.cached.json
# RUN: grep -q "^  - Entry: \"$(cat %t.next)\"" %t.p/revng.yml
# RUN: sed -i "s/^  - Entry: \"$(cat %t.next)\"/  - Entry: \"$(cat %t.inside)\"\n&/" %t.p/revng.yml
# RUN: cd %t.p && env REVNG_OPTIONS=--trace=%t.added.json %root/bin/revng project --storage-provider 'local://?inline' artifact lift -o %t.added.bc
# RUN: grep -q '"name": "Running lift", "cat": "task", "ph": "B"' %t.added.json

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
    mov esi, edi
    call add2
    ret
