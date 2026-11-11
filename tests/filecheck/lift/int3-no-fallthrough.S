#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` executes int3 when rdi is zero, and a call to `after_trap` follows
# the int3. The program cannot continue after int3, so nothing reaches the
# call and `after_trap`, with no symbol, is not a function. The lifter used to
# translate the bytes after a translation block ending in int3 as code all the
# same, which made `after_trap` a function, and lifted padding of int3 one byte
# at a time: in a slice of Warcraft III, where junk paths of the protected code
# reach such padding, that registered 1.66 million jump targets for three
# functions of 200 bytes and took the lift past 40 GB.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-s -Wl,-T,%S/../detect-abi/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x401100:Code_x86_64"
# CHECK-NOT: Entry: "0x401200:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl entry
.type entry, @function
entry:
  test %rdi, %rdi
  je 1f
  ret
1:
  int3
  call after_trap
  ret
.size entry, .-entry

.p2align 8
after_trap:
  mov $7, %eax
  ret
