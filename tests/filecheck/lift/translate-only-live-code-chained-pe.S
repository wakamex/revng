#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A function's parts in a PE file's exception directory (.pdata) are the
# entries whose unwind information chains to its first one, however its code
# flows between them: here `start` runs on into its chained part without a
# jump. With -translate-only-live-code the walk of `start` covers that part, so
# `live`, the target of the jump the part always takes, is translated, and
# `dead`, whose address `start` stores, is not.
#
# RUN: clang --target=x86_64-pc-windows-msvc -c %s -o %t.obj
# RUN: lld-link /entry:start /subsystem:console /nodefaultlib /out:%t.exe %t.obj
# RUN: env REVNG_OPTIONS='-prune-decided-branches -translate-only-live-code -debug-log=dead-code' %root/bin/revng quick artifact lift --analyses=parse-binary %t.exe -o %t.bc 2> %t.log
# RUN: FileCheck %s --check-prefix=LOG < %t.log
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=IR < %t.ll
#
# LOG: Not translating 0x140001040:Code_x86_64
# LOG-NOT: Not translating 0x140001080:Code_x86_64
# IR: "bb.0x140001080:Code_x86_64":

.text
.globl start
.def start; .scl 2; .type 32; .endef
start:
  sub $0x28, %rsp
  lea dead(%rip), %rax
  mov %rax, slot(%rip)
part:
  stc
  jb live
.p2align 6
dead:
  mov $1, %eax
  add $0x28, %rsp
  ret
.p2align 6
live:
  xor %eax, %eax
  add $0x28, %rsp
  ret
end:

# The exception directory, written out as MSVC lays it out: an entry for the
# function's first part, and one for the rest whose unwind information chains
# to it
.section .pdata, "dr"
  .rva start, part, start_unwind
  .rva part, end, part_unwind

.section .xdata, "dr"
.p2align 2
start_unwind:
  # Version 1, a prologue of 4 bytes and one code: at 4, sub $0x28, %rsp
  .byte 0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00
part_unwind:
  # Version 1 with UNW_FLAG_CHAININFO, no codes, then the chained entry
  .byte 0x21, 0x00, 0x00, 0x00
  .rva start, part, start_unwind

.data
slot:
  .quad 0
