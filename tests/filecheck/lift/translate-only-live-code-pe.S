#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# With -translate-only-live-code, code of a function that no path of it reaches
# is not translated, however its address is found. `start` stores the address
# of `dead`, and the jump that skips it is always taken after stc, so no path
# of `start` reaches it.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code -debug-log=dead-code' %root/bin/revng quick artifact lift --analyses=parse-binary %t.exe -o %t.bc 2> %t.log
# RUN: FileCheck %s --check-prefix=LOG < %t.log
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=IR < %t.ll
#
# LOG: Not translating 0x140001040:Code_x86_64
# IR: "bb.0x140001000:Code_x86_64":
# IR-NOT: "bb.0x140001040:Code_x86_64":

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  lea dead(%rip), %rax
  mov %rax, slot(%rip)
  stc
  jb live
.p2align 6
dead:
  mov $1, %eax
  add $0x28, %rsp
  ret
live:
  xor %eax, %eax
  add $0x28, %rsp
  ret
.seh_endproc

.data
slot:
  .quad 0
