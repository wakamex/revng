#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# A raw pointer into a jump's operand starts an invalid decode that eventually
# reaches rdpmc in the operand of the following call. An unwind-based sweep
# rejects that speculative start before translation. Real instruction starts,
# code without unwind information, and addresses past a failed sweep survive.
# A direct branch can still establish an overlapping instruction stream.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/jump-table-extent.ld %s -o %t
# RUN: env REVNG_OPTIONS='-debug-log=registerjt -debug-log=raw-code-pointers' %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc 2> %t.log
# RUN: FileCheck %s --check-prefix=LOG < %t.log
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: FileCheck %s --check-prefix=IR --implicit-check-not='call void @helper_rdpmc' < %t.ll
#
# LOG-DAG: Ignoring raw code pointer at 0x402000:Generic64 to instruction interior 0x401011:Code_x86_64
# LOG-DAG: Registering bb.0x401030:Code_x86_64 for GlobalData
# LOG-DAG: Registering bb.0x401051:Code_x86_64 for GlobalData
# LOG-DAG: Registering bb.0x401072:Code_x86_64 for GlobalData
# LOG-DAG: Ignoring raw code pointer at 0x402020:Generic64 to instruction interior 0x4010a2:Code_x86_64
# LOG-DAG: Registering bb.0x4010a2:Code_x86_64 for DirectJump
# IR-DAG: "bb.0x401030:Code_x86_64":
# IR-DAG: "bb.0x401051:Code_x86_64":
# IR-DAG: "bb.0x401072:Code_x86_64":
# IR-DAG: "bb.0x4010a2:Code_x86_64":

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  call direct_overlap
  mov $42, %eax
  ret
  .org 0x10
operand_stream:
  .byte 0xe9, 0x5f, 0xff, 0xff, 0xff
  .byte 0xe8, 0x5f, 0x0f, 0x33, 0x00
  ret
  .cfi_endproc
.size entry, .-entry

.org 0x30
valid_start:
  .cfi_startproc
  mov $7, %eax
  ret
  .cfi_endproc

.org 0x50
without_unwind:
  .byte 0x48
  mov $8, %eax
  ret

.org 0x70
failed_sweep:
  .cfi_startproc
  .byte 0x06, 0x06
  mov $9, %eax
  ret
  .cfi_endproc

.org 0x90
.type direct_overlap, @function
direct_overlap:
  .cfi_startproc
  jmp overlapping+2
  .org 0xa0
overlapping:
  .byte 0x48, 0xb8, 0xc3, 0, 0, 0, 0, 0, 0, 0
  ret
  .cfi_endproc
.size direct_overlap, .-direct_overlap

.section .rodata
.balign 8
  .quad operand_stream+1
  .quad valid_start
  .quad without_unwind+1
  .quad failed_sweep+2
  .quad overlapping+2
  .quad 0

.section .note.GNU-stack,"",@progbits
