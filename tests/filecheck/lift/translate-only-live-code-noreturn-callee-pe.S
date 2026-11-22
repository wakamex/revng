#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# MSVC ends a function with `call f; nop` when f does not return, so that the
# return address stays in the function's unwind range (.pdata). That makes f a
# function that does not return at every call: with -translate-only-live-code,
# where such a call would return is not translated, whatever follows it, here
# the end of an epilogue and a leaf function with no unwind information. Code
# after such a call that a jump reaches is still translated.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code' %root/bin/revng quick artifact lift --analyses=parse-binary %t.exe -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=IR < %t.ll
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code' %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t.exe | FileCheck %s --check-prefix=MODEL
#
# start's jne reaches 0x14000100e, right after its first call to thrower
# IR-DAG: "bb.0x14000100e:Code_x86_64":
# IR-NOT: "bb.0x140001018:Code_x86_64":
# IR-NOT: "bb.0x14000102b:Code_x86_64":
# IR-NOT: "bb.0x14000102c:Code_x86_64":
# MODEL: Entry: "0x140001000:Code_x86_64"
# MODEL: Entry: "0x140001020:Code_x86_64"
# MODEL: Entry: "0x140001040:Code_x86_64"

.text
.globl start
.def start; .scl 2; .type 32; .endef
.seh_proc start
start:
  push %rbx
  .seh_pushreg %rbx
  sub $0x20, %rsp
  .seh_stackalloc 0x20
  .seh_endprologue
  test %ecx, %ecx
  jne shared
  call thrower
shared:
  call other
  call thrower
  mov 0x50(%rsp), %rbx
.seh_endproc
  int3

.p2align 4, 0xcc
.def other; .scl 2; .type 32; .endef
.seh_proc other
other:
  sub $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  xor %ecx, %ecx
  call thrower
  nop
.seh_endproc
leaf:
  mov 0x30(%rdx), %rcx
  ret

.p2align 4, 0xcc
.def thrower; .scl 2; .type 32; .endef
.seh_proc thrower
thrower:
  .seh_endprologue
  mov $1, %eax
  ret
.seh_endproc
