#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` executes `int 0x29`, the __fastfail of Windows programs, when rdi is
# zero and icebp (`int1`) when rsi is zero, each followed by a call. Neither
# exception returns to the program, so nothing reaches the calls and their
# callees, with no symbol, are not functions. The lifter used to translate the
# bytes after both as code, as it did after int3.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-s -Wl,-T,%S/../detect-abi/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x401100:Code_x86_64"
# CHECK-NOT: Entry: "0x401200:Code_x86_64"
# CHECK-NOT: Entry: "0x401300:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl entry
.type entry, @function
entry:
  test %rdi, %rdi
  je 1f
  test %rsi, %rsi
  je 2f
  ret
1:
  int $0x29
  call after_fastfail
  ret
2:
  .byte 0xf1
  call after_icebp
  ret
.size entry, .-entry

.p2align 8
after_fastfail:
  mov $7, %eax
  ret

.p2align 8
after_icebp:
  mov $8, %eax
  ret
