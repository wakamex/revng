#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# MSVC ends a function with `call f; nop; int3` when f does not return, and the
# function's unwind range (.pdata) ends before the int3. With
# -translate-only-live-code, the nop where the call would return and the int3
# are not translated, even though `thrower` itself returns as far as its code
# shows, as a function raising an exception through a returning import does.
# detect-abi still runs on the result.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code -debug-log=dead-code' %root/bin/revng quick artifact lift --analyses=parse-binary %t.exe -o %t.bc 2> %t.log
# RUN: FileCheck %s --check-prefix=LOG < %t.log
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=IR < %t.ll
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s --check-prefix=MODEL
#
# LOG: Not registering 0x140001009:Code_x86_64: it follows a call that does not return
# IR: "bb.0x140001000:Code_x86_64":
# IR-NOT: "bb.0x140001009:Code_x86_64":
# IR-NOT: 0x14000100a
# MODEL: Entry: "0x140001000:Code_x86_64"
# MODEL: Entry: "0x140001010:Code_x86_64"

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  call thrower
  nop
.seh_endproc

.p2align 4, 0xcc
.def thrower; .scl 2; .type 32; .endef
.seh_proc thrower
thrower:
  .seh_endprologue
  mov $1, %eax
  ret
.seh_endproc
