#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` divides the 128-bit value in its first two arguments by the third.
# The division helper shifts one quotient bit in per step, and a step whose
# remainder has its top bit set takes the bit from a 1-bit `true`. Integer
# constants narrower than their C type must be zero-extended into it: the C
# stores the bit in an 8-bit variable and ORs it into the quotient, so a `true`
# printed as -1 sets eight quotient bits.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: do {
# CHECK-NOT: (generic8_t) -1
# CHECK: generic8_t [[BIT:var_[0-9]+]] = (int64_t) {{.*}} < (int64_t) 0U ? {{.*}}1 :
# CHECK-NOT: (generic8_t) -1
# CHECK: << 1UL | [[BIT]];
# CHECK: } while

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  mov %rdx, %rcx
  mov %rdi, %rax
  mov %rsi, %rdx
  div %rcx
  ret
  .cfi_endproc
.size entry, .-entry
