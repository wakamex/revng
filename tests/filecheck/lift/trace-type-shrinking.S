#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# Each harvest round reports, before type shrinking, the temporary root's
# blocks and instructions and, when LLVM provides the LazyValueInfo cache
# accessors of the fork's LLVM patch, the cache JumpThreading leaves: its
# cached blocks and tracked values and the time of a few sampled erasures.
# After type shrinking it then reports how many tracked values were removed.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/../detect-abi/overlapping-indirect-branch.ld %s -o %t
# RUN: env REVNG_OPTIONS=--trace=%t.trace %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: FileCheck %s < %t.trace
#
# CHECK: "name": "Harvest with value materialization", "cat": "task", "ph": "B"
# CHECK: "name": "MeasureLazyValueInfoPass", "cat": "task", "ph": "B"
# CHECK-NEXT: "name": "Lazy value info before type shrinking", "cat": "task", "ph": "B"
# CHECK: "name": "{{[0-9]+}} blocks, {{[0-9]+}} instructions{{(, [0-9]+ cached blocks, [0-9]+ tracked values, [0-9]+ erases in [0-9]+ ns)?}}", "cat": "task", "ph": "B", "args": {"step": 0, "steps": 1}
# CHECK: "name": "TypeShrinking::TypeShrinkingPass", "cat": "task", "ph": "B"
# CHECK: "name": "CountTypeShrinkingRemovalsPass", "cat": "task", "ph": "B"

.text
.globl entry
.type entry, @function
entry:
    lea target(%rip), %rax
    test %rdi, %rdi
    je 1f
    lea other(%rip), %rax
1:
    jmp *%rax
target:
    mov $1, %eax
    ret
other:
    mov $2, %eax
    ret
