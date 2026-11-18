#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -functions-from-unwind-entries, a leaf function of a PE program has no
# entry in the exception directory (.pdata), and a conditional tail call may be
# the only way to it. `start` ends in `jne leaf`: `leaf` follows int3 padding at
# a 16-byte boundary, so it is a function. The blocks `leaf` branches to are not.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-functions-from-unwind-entries' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x140001000:Code_x86_64"
# CHECK: Entry: "0x140001040:Code_x86_64"
# CHECK-NOT: Entry: "0x14000104{{[1-9a-f]}}:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  .seh_endprologue
  test %ecx, %ecx
  jne leaf
  ret
.seh_endproc

.p2align 6, 0xcc
leaf:
  test %edx, %edx
  je leaf_zero
  mov $1, %eax
  ret
leaf_zero:
  xor %eax, %eax
  ret
