#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` is guarded the way Warcraft III's protector guards 28,850 places in
# its code: `cmp esp, imm32; ja; sub esp, imm8`. A write to the 32-bit ESP
# zeroes the upper half of RSP, so in 64-bit code it is taken only by junk
# paths that real stacks never reach. Translated, such a path joins the real
# one with an unknown stack, the function's stack offset at `ret` cannot be
# elected, and its return became a longjmp. The path now ends at the write.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-s -Wl,-T,%S/../detect-abi/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: longjmp
# CHECK: return 7

.text
.globl entry
.type entry, @function
entry:
  cmp $0x5000, %esp
  ja 1f
  sub $0x10, %esp
1:
  mov $7, %eax
  ret
.size entry, .-entry
