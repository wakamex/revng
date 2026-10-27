#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A helper call passes a null out argument for each CPU-state variable it
# writes that the function never reads. `syscall` keeps the helper call,
# whose null arguments the C prints as `NULL`. The Clifter used to import a
# null pointer as the immediate 64, the pointer size in bits.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: helper_syscall{{.*}}, NULL{{[,)]}}
# CHECK-NOT: (void *) 64UL

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
  mov rax, rdi
  xor edx, edx
  div rsi
  ret
