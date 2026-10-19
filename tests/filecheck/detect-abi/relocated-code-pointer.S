#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `pointed` is reached only through a pointer in data, as a method is through
# a vtable. The binary is position independent and linked by lld, which leaves
# the stored word zero and puts the address in a base-relative relocation's
# addend; without symbols, that relocation is the only way to find `pointed`.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Functions:
# CHECK-DAG: - Entry: "0x40126c:Code_x86_64"
# CHECK-DAG: - Entry: "0x40127c:Code_x86_64"

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  mov $1, %eax
  ret
  .cfi_endproc
.size entry, .-entry

.org 0x10
.type pointed, @function
pointed:
  .cfi_startproc
  mov $7, %eax
  ret
  .cfi_endproc
.size pointed, .-pointed

.section .data.rel.ro,"aw"
.quad pointed
