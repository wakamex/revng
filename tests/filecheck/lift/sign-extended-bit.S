#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` returns -1 when its first argument is below the second and 0
# otherwise: `sbb` subtracts the carry from zero, which lifts to a 1-bit
# comparison sign-extended to 64 bits. The C holds a 1-bit value as 0 or 1, so
# sign extension negates it: without that the C returned 1.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: return -{{.*}}(argument_0 < argument_1);

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  cmp %rsi, %rdi
  sbb %rax, %rax
  ret
  .cfi_endproc
.size entry, .-entry
