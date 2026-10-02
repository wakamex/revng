#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The binary is stripped, so `second` and `first` become functions only because
# they are called; the linker script keeps the ELF header out of the code. Each
# stage of detect-abi that can take long on a large binary names itself in the
# trace: the collection of these callees, the preliminary analysis with one
# step per function and the size of each function, and the creation of the
# temporary functions.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,--strip-all -Wl,-T,%S/overlapping-indirect-branch.ld %s -o %t
# RUN: env REVNG_OPTIONS=--trace=%t.trace %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t > %t.yml
# RUN: FileCheck %s --check-prefix=TRACE < %t.trace
# RUN: FileCheck %s --check-prefix=MODEL < %t.yml
#
# TRACE: "name": "Collect functions from callees", "cat": "task", "ph": "B"
# TRACE: "name": "Scan {{[0-9]+}} blocks of root", "cat": "task", "ph": "B", "args": {"step": 0, "steps": 2}
# TRACE: "name": "Create 2 functions", "cat": "task", "ph": "B", "args": {"step": 1, "steps": 2}
# TRACE: "name": "Prepare the ABI detection", "cat": "task", "ph": "B"
# TRACE: "name": "preliminaryFunctionAnalysis", "cat": "task", "ph": "B"
# TRACE: "name": "Analyze functions", "cat": "task", "ph": "B"
# TRACE: "name": "0x{{[0-9a-f]+}}:Code_x86_64", "cat": "task", "ph": "B", "args": {"step": 0, "steps": 3}
# TRACE: "name": "Outline", "cat": "task", "ph": "B"
# TRACE: "name": "Prepare ({{[0-9]+}} instructions, {{[0-9]+}} calls, {{[0-9]+}} blocks)"
# TRACE: "name": "Optimize ({{[0-9]+}} instructions, {{[0-9]+}} calls, {{[0-9]+}} blocks)"
# TRACE: "name": "Collect results ({{[0-9]+}} instructions, {{[0-9]+}} calls, {{[0-9]+}} blocks)"
# TRACE: "name": "0x{{[0-9a-f]+}}:Code_x86_64", "cat": "task", "ph": "B", "args": {"step": 2, "steps": 3}
# TRACE: "name": "Outline functions", "cat": "task", "ph": "B"
# TRACE: "name": "0x{{[0-9a-f]+}}:Code_x86_64", "cat": "task", "ph": "B", "args": {"step": 2, "steps": 3}
# TRACE: "name": "Collect functions from unused addresses", "cat": "task", "ph": "B"
#
# MODEL: Functions:
# MODEL-COUNT-3: - Entry: "0x{{[0-9a-f]+}}:Code_x86_64"
# MODEL-NOT: - Entry:
# MODEL: TypeDefinitions:

.text
.globl entry
.type entry, @function
entry:
  call second
  call first
  ret
second:
  call first
  ret
first:
  ret
.section .note.GNU-stack,"",@progbits
