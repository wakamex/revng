#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -functions-from-unwind-entries, an address of code of a PE program
# outside every range of its exception directory (.pdata), where leaf
# functions lie, that code computes as a constant, as it does a callback's, is
# a function entry, even when code also jumps to it as a tail call. `start`
# passes `leaf1`'s address to `register` in a register, without storing it, and
# jumps to it. The blocks `leaf1` branches to are not functions: their addresses
# are constants too, but they lie in `leaf1`'s control flow graph.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-functions-from-unwind-entries' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x140001000:Code_x86_64"
# CHECK: Entry: "0x140001040:Code_x86_64"
# CHECK-NOT: Entry: "0x14000104{{[1-9a-f]}}:Code_x86_64"
# CHECK: Entry: "0x140001080:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  lea leaf1(%rip), %rcx
  call register
  add $0x28, %rsp
  jmp leaf1
.seh_endproc

.p2align 6, 0xcc
leaf1:
  mov $1, %eax
  test %ecx, %ecx
  jne leaf1_inner
  ret
leaf1_inner:
  mov $2, %eax
  ret

.p2align 6, 0xcc
.def register; .scl 3; .type 32; .endef
.seh_proc register
register:
  .seh_endprologue
  ret
.seh_endproc
