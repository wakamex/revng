#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A load through the fs segment adds the fs base, which no instruction of the
# program writes. Promoting the CPU state variables to locals must not
# initialize the fs and gs bases from their static initializer, zero: the C
# would then read thread-local data from absolute addresses. They take an
# opaque value instead.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: revng_undefined__fs_base()

.intel_syntax noprefix
.text
.globl _start
_start:
  call f
  ud2

.globl f
.type f,@function
f:
  mov rax, qword ptr fs:[0x10]
  ret
