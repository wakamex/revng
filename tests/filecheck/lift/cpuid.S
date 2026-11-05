#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `cpuid` translates to helper_cpuid, which in the helper bitcode of the
# wakamex/qemu fork asks the runtime's revng_cpuid for the host's answers and
# touches no CPU state besides EAX to EDX. The helper is inlined and the C
# emitted for the function calls revng_cpuid with the leaf and subleaf. With
# QEMU's own helper_cpuid the CPU state escaped to cpu_x86_cpuid, and the C
# called helper_cpuid with a made-up CPU state address.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: helper_cpuid
# CHECK: revng_cpuid(argument_0, 0U,
# CHECK-NOT: helper_cpuid

.intel_syntax noprefix
.text
.globl _start
_start:
  call f
  ud2

.globl f
.type f,@function
f:
  push rbx
  mov eax, edi
  xor ecx, ecx
  cpuid
  mov eax, ecx
  pop rbx
  ret
