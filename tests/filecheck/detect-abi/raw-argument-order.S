#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `pair` takes `rdi` and `rsi` and returns values in both `rax` and `xmm0`,
# which `entry` adds. No C ABI return value occupies a general purpose and a
# vector register together, so `pair`'s prototype stays raw. Its parameters come in the order the default
# ABI passes arguments in, `rdi` before `rsi`: C calling `pair` through a
# pointer to a function of two integers then passes its first argument in
# `rdi`, as the machine code expects. In register enumeration order `rsi`
# came first. The call in `entry` passes the arguments in the same order.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: = function_0x{{[0-9a-f]+}}_Code_x86_64(7UL, 5UL);
# CHECK: _ABI(raw_x86_64)
# CHECK-NEXT: function_0x{{[0-9a-f]+}}_Code_x86_64(generic64_t register_rdi _REG(rdi_x86_64), generic64_t register_rsi _REG(rsi_x86_64)) {
# CHECK-NEXT: register_rdi - register_rsi

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  mov $7, %edi
  mov $5, %esi
  call pair
  movq %xmm0, %rdx
  add %rdx, %rax
  ret
  .cfi_endproc
.size entry, .-entry

.type pair, @function
pair:
  .cfi_startproc
  mov %rdi, %rax
  sub %rsi, %rax
  movq %rsi, %xmm0
  ret
  .cfi_endproc
.size pair, .-pair
