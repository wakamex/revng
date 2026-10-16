#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `host` ends in an exit system call, which the analysis does not know to be
# no-return, so its control-flow graph falls through the padding into the
# code at 0x401020. That code has unwind information of its own and its address
# is stored in data, as a method is in a vtable: it is a function. Without the
# unwind information an address inside another function's graph is taken to
# be part of that function, and 0x401020 gets no function.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/../lift/jump-table-extent.ld %s -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Functions:
# CHECK-DAG: - Entry: "0x401010:Code_x86_64"
# CHECK-DAG: - Entry: "0x401020:Code_x86_64"

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call .Lhost
  ret
  .cfi_endproc
.size entry, .-entry

.org 0x10
.Lhost:
  .cfi_startproc
  mov $60, %eax
  syscall
  .cfi_endproc
  .org 0x20, 0x90

.Lpointed:
  .cfi_startproc
  mov $7, %eax
  ret
  .cfi_endproc

.section .rodata
.quad .Lpointed
