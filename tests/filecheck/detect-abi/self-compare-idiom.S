#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `leaf` sets `xmm0` to all ones with `pcmpeqd %xmm0, %xmm0`, which compares
# each 32-bit lane of the register with itself, and returns its low half in
# `rax`. The result does not depend on what `xmm0` held, so `leaf` reads no
# argument from it. It used to take `zmm0`: each lane was extracted twice from
# the register to be compared, and the comparison was folded only later.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: register_zmm0 _REG
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(void) {
# CHECK-NOT: register_zmm0 _REG

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call leaf
  add $1, %rax
  ret
  .cfi_endproc
.size entry, .-entry

.type leaf, @function
leaf:
  .cfi_startproc
  pcmpeqd %xmm0, %xmm0
  movq %xmm0, %rax
  ret
  .cfi_endproc
.size leaf, .-leaf
