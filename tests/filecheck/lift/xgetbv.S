#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `xgetbv` translates to helper_xgetbv, which in the helper bitcode of the
# wakamex/qemu fork asks the runtime's revng_xgetbv for the host's extended
# control register, as helper_cpuid asks revng_cpuid for CPUID. The helper is
# inlined and the C emitted for the function calls revng_xgetbv with ECX. With
# QEMU's own helper_xgetbv, which reads the emulated control registers through
# the CPU state, the C called helper_xgetbv_wrapper with every one of them
# undefined, and the recovered code aborted at the first: CPU dispatch code
# runs `xgetbv` to learn whether the OS saves the AVX registers.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: helper_xgetbv
# CHECK: revng_xgetbv(0U)
# CHECK-NOT: helper_xgetbv

.intel_syntax noprefix
.text
.globl _start
_start:
  call f
  ud2

.globl f
.type f,@function
f:
  xor ecx, ecx
  xgetbv
  shl rdx, 32
  or rax, rdx
  ret
