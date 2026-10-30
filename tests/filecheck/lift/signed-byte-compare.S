#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` branches on the sign of the byte `rdi` points to, as Rust's
# formatting does to tell template bytes from arguments. The comparison is
# signed and the byte has an unsigned C type, so the C casts it to `int8_t`.
# The cast changes values, so it is not left implicit: both sides of a
# comparison are promoted to `int`, and an unsigned byte compared with 0 is
# never negative. It used to print `(generic8_t) x < (generic8_t) 0`.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: (int8_t) {{.*}} < (int8_t)

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  movzbl (%rdi), %eax
  test %al, %al
  js 1f
  lea (%rax,%rax,2), %rax
  ret
1:
  sub $5, %rax
  imul %rsi, %rax
  ret
  .cfi_endproc
.size entry, .-entry
