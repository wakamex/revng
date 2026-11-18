#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# An MMX instruction can raise an exception. The message of the helper that
# raises it is a constant of the helper module and the C emitted for the
# function prints it as a string literal. Without its content C emission aborted
# in the Clifter's string literal detection.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: revng_abort({{.*}}"Exception raised by helper_raise_exception")

.intel_syntax noprefix
.text
.globl _start
_start:
  call f
  ud2

.globl f
.type f,@function
f:
  punpckldq mm0, dword ptr [rdi]
  movq rax, mm0
  ret
