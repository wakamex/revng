#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A call into the middle of a function a PE file's exception directory (.pdata)
# describes creates no function there: code does not call into the middle of
# another function, and such a call is junk, as obfuscators insert. `start`
# calls `mid`, 16 bytes into `other`, on a path that depends on its argument.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x140001100:Code_x86_64"
# CHECK-NOT: Entry: "0x140001110:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  test %ecx, %ecx
  je 1f
  call mid
1:
  call other
  add $0x28, %rsp
  ret
.seh_endproc

.p2align 8
.def other; .scl 3; .type 32; .endef
.seh_proc other
other:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  .fill 12, 1, 0x90
mid:
  mov $1, %eax
  add $0x28, %rsp
  ret
.seh_endproc
