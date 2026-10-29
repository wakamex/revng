#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` sums the array of its second argument's length that starts at its
# first argument, walking down from the last element. The pointer to that
# element is the array plus a scaled index minus 8. Pointer arithmetic takes a
# signed offset: adding an unsigned integer that holds -8 overflows the
# pointer in C, which a compiler may assume does not happen.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: do {
# CHECK-NOT: + -8UL
# CHECK: (generic8_t *) argument_0 + (int64_t) (argument_1 << 3UL) + -8L
# CHECK: } while

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  lea (%rdi,%rsi,8), %rax
  xor %ecx, %ecx
1:
  add -8(%rax), %rcx
  sub $8, %rax
  cmp %rdi, %rax
  jne 1b
  mov %rcx, %rax
  ret
  .cfi_endproc
.size entry, .-entry
