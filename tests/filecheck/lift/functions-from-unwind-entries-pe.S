#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -functions-from-unwind-entries, every function entry a PE file's
# exception directory (.pdata) describes is a function, even when nothing
# refers to it, as when a program reaches it only through pointers it builds
# at run time. `other` is such a function.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-functions-from-unwind-entries' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s
#
# CHECK-LABEL: {{^}}Functions:
# CHECK: Entry: "0x140001000:Code_x86_64"
# CHECK: Entry: "0x140001100:Code_x86_64"
# CHECK: {{^}}TypeDefinitions:

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  xor %eax, %eax
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
  mov $1, %eax
  add $0x28, %rsp
  ret
.seh_endproc
