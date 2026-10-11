#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# unwind-entry-after-call.S in an x86-64 PE file, whose function ranges come
# from the exception directory (.pdata) instead of .eh_frame. `after` is
# reached only through `table` and starts right after `fail`, whose last
# instruction calls `stop`, which traps. Without the function ranges, the
# address after a call is only a return address, and `after` is no function.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t.exe -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t function_0x{{[0-9a-f]+}}_Code_x86_64(void) {
# CHECK-NEXT: return 7

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  call *table(%rip)
  add $0x28, %rsp
  ret
.seh_endproc

.def stop; .scl 3; .type 32; .endef
.seh_proc stop
stop:
  .seh_endprologue
  ud2
.seh_endproc

.def fail; .scl 3; .type 32; .endef
.seh_proc fail
fail:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  call stop
.seh_endproc

.def after; .scl 3; .type 32; .endef
.seh_proc after
after:
  .seh_endprologue
  mov $7, %eax
  ret
.seh_endproc

.data
table:
  .quad after
  .quad fail
