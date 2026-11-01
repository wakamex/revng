#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` pushes three stack arguments on each of two paths, one of them
# copying a slot of its frame, before both paths reach the same call to
# `callee`, which reads its stack arguments. Where the paths meet, the stack
# pointer is a PHI of two equal offsets from its entry value. Accesses through
# it count as direct stack accesses, so the return address `ret` jumps to is
# known to be the one `entry` was called with, and `entry` returns. They used
# to share alias information with every other memory access, the callee's
# writes could reach the return address, and `entry` became a function that
# never returns: Rust's exact float formatting has this shape, and its C ended
# every call in an abort.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK-NOT: NoReturn
# CHECK: Name: entry
# CHECK-NOT: NoReturn

.text
.globl entry
.type entry, @function
entry:
  push %rbp
  mov %rsp, %rbp
  push %rbx
  sub $0x18, %rsp
  mov %rsi, -0x18(%rbp)
  mov %rdi, %rbx
  test %rdi, %rdi
  jz 1f
  sub $8, %rsp
  push %rax
  push %rbx
  push %rdi
  jmp 2f
1:
  sub $8, %rsp
  push -0x18(%rbp)
  push %rsi
  push %rax
2:
  call callee
  add $0x20, %rsp
  mov %rbx, %rax
  add $0x18, %rsp
  pop %rbx
  pop %rbp
  ret
.size entry, .-entry

.type callee, @function
callee:
  mov 8(%rsp), %rax
  add 16(%rsp), %rax
  ret
.size callee, .-callee
