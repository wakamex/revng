#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The CPU-state analysis reports the helper of `div` as writing r12, which it
# can leave unchanged, and not as reading it. Its wrapper must still pass r12
# through, so that the comparison after the division compares the value r12
# held before it. Without that, r12 read a stale value after the call, and the
# comparison and its return of 105 disappeared from the C.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-LABEL: {{^}}{{.*}} f(generic64_t
# CHECK: 105

.intel_syntax noprefix
.text
.globl _start
_start:
  call f
  mov rdi, rax
  mov eax, 60
  syscall
  ud2

.globl f
.type f,@function
f:
  push r12
  mov r12, rdi
  mov rax, rdi
  xor edx, edx
  div rsi
  cmp rsi, r12
  ja 1f
  pop r12
  ret
1:
  mov eax, 105
  pop r12
  ret
