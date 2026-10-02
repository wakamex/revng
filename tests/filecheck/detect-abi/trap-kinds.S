#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Each tN returns unless rdi is zero, in which case it executes the
# instruction under test; vN follows it after nop padding. An instruction that
# faults on the machine ends the control-flow graph of tN with an unreachable
# edge, while one after which execution continues falls through the padding
# into vN. QEMU raises #UD for the undefined opcodes and for valid instructions
# it does not implement alike, so the lifter tells them apart by their bytes.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick artifact emit-cfg --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# ud2
# CHECK-LABEL: /function/0x401040:Code_x86_64:
# CHECK-NOT: 0x401050:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x401050:Code_x86_64
# CHECK-LABEL: /function/0x401050:Code_x86_64:
# ud1
# CHECK-LABEL: /function/0x401060:Code_x86_64:
# CHECK-NOT: 0x401070:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x401070:Code_x86_64
# CHECK-LABEL: /function/0x401070:Code_x86_64:
# ud0
# CHECK-LABEL: /function/0x401080:Code_x86_64:
# CHECK-NOT: 0x401090:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x401090:Code_x86_64
# CHECK-LABEL: /function/0x401090:Code_x86_64:
# int3
# CHECK-LABEL: /function/0x4010a0:Code_x86_64:
# CHECK-NOT: 0x4010b0:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x4010b0:Code_x86_64
# CHECK-LABEL: /function/0x4010b0:Code_x86_64:
# int $3
# CHECK-LABEL: /function/0x4010c0:Code_x86_64:
# CHECK-NOT: 0x4010d0:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x4010d0:Code_x86_64
# CHECK-LABEL: /function/0x4010d0:Code_x86_64:
# hlt
# CHECK-LABEL: /function/0x4010e0:Code_x86_64:
# CHECK-NOT: 0x4010f0:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x4010f0:Code_x86_64
# CHECK-LABEL: /function/0x4010f0:Code_x86_64:
# ud2 with an operand-size prefix
# CHECK-LABEL: /function/0x401100:Code_x86_64:
# CHECK-NOT: 0x401110:Code_x86_64
# CHECK: Unreachable
# CHECK-NOT: 0x401110:Code_x86_64
# CHECK-LABEL: /function/0x401110:Code_x86_64:
# int $0x80, a system call
# CHECK-LABEL: /function/0x401120:Code_x86_64:
# CHECK-NOT: Unreachable
# CHECK: 0x401128:Code_x86_64
# CHECK-LABEL: /function/0x401130:Code_x86_64:
# syscall
# CHECK-LABEL: /function/0x401140:Code_x86_64:
# CHECK-NOT: Unreachable
# CHECK: 0x401148:Code_x86_64
# CHECK-LABEL: /function/0x401150:Code_x86_64:
# an AVX-512 instruction, valid but raising #UD in the lifter
# CHECK-LABEL: /function/0x401160:Code_x86_64:
# CHECK-NOT: Unreachable
# CHECK: 0x401168:Code_x86_64
# CHECK-LABEL: /function/0x401170:Code_x86_64:

.macro case n, insn:vararg
.p2align 5
.globl t\n
.type t\n, @function
t\n:
  test %rdi, %rdi
  je 1f
  ret
1:
  \insn
.size t\n, .-t\n
.p2align 4
.globl v\n
.type v\n, @function
v\n:
  mov $0x11, %eax
  ret
.size v\n, .-v\n
.endm

.text
.globl entry
.type entry, @function
entry:
  ret
.size entry, .-entry

case 0, ud2
case 1, ud1 %eax, %eax
case 2, .byte 0x0f, 0xff, 0xc0
case 3, int3
case 4, .byte 0xcd, 0x03
case 5, hlt
case 6, .byte 0x66, 0x0f, 0x0b
case 7, int $0x80
case 8, syscall
case 9, vpaddd %zmm2, %zmm1, %zmm0
.section .note.GNU-stack,"",@progbits
