#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` traps with ud2 when its argument is zero. The trap's block ends the
# path, and removing the call that raises the exception must not leave a bare
# unreachable, which would let the optimizer delete the path and the test of
# the argument: the C keeps the test and aborts.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: if (!argument_0)
# CHECK-NEXT: revng_abort({{.*}}"Exception raised by helper_raise_exception");
# CHECK-NEXT: return 7UL;

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  test %rdi, %rdi
  jne 1f
  ud2
1:
  mov $7, %eax
  ret
  .cfi_endproc
.size entry, .-entry
