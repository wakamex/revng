#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The initial values of CSVs come from libtcg's CPU, whose setup maps memory for
# the x86 descriptor tables and stores their addresses in the CPU state. Those
# addresses depend on the memory layout of the process, so neither the helper
# modules nor the lifted module may take them. Lift twice without address
# randomization, once with the default stack limit and once with an unlimited
# one, which makes Linux place mappings bottom-up, and require the same module.
#
# The GDT base CSV comes from the helper modules the build generated, the IDT
# base CSV is created by the lift.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: sh -c 'ulimit -s 8192 && exec setarch -R %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.default.bc'
# RUN: sh -c 'ulimit -s unlimited && exec setarch -R %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.unlimited.bc'
# RUN: cmp %t.default.bc %t.unlimited.bc
# RUN: llvm-dis %t.default.bc -o - | FileCheck %s
#
# CHECK-DAG: @_state_0x2940 = global i64 0,
# CHECK-DAG: @_state_0x2958 = global i64 0,

.text
.globl _start
.type _start, @function
_start:
    sgdt -16(%rsp)
    sidt -32(%rsp)
    mov -14(%rsp), %rax
    add -30(%rsp), %rax
    ret
