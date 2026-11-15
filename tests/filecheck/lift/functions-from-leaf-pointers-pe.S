#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -functions-from-unwind-entries, a pointer in data to code of a PE
# program outside every range of its exception directory (.pdata), where leaf
# functions lie, is a function entry, as a vtable's pointer to a leaf is, even
# when code loads the pointer or jumps to the leaf as a tail call. `start`
# calls `leaf1` through `table` and jumps to `leaf2`, which `table` also holds,
# and stores the address of `vtable`, as a constructor does, whose only entry
# is `leaf3`.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-functions-from-unwind-entries' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x140001000:Code_x86_64"
# CHECK: Entry: "0x140001040:Code_x86_64"
# CHECK: Entry: "0x140001050:Code_x86_64"
# CHECK: Entry: "0x140001060:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  call *table(%rip)
  lea vtable(%rip), %rax
  mov %rax, object(%rip)
  add $0x28, %rsp
  jmp leaf2
.seh_endproc

.p2align 6, 0xcc
leaf1:
  mov $1, %eax
  ret

.p2align 4, 0xcc
leaf2:
  mov $2, %eax
  ret

.p2align 4, 0xcc
leaf3:
  mov $3, %eax
  ret

.section .rdata, "dr"
.p2align 3
table:
  .quad leaf1
  .quad leaf2
vtable:
  .quad leaf3

.data
object:
  .quad 0
