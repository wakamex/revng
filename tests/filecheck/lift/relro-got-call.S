#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` calls `panic`, which never returns, and `work` through their GOT
# slots, as code compiled without a PLT calls functions of other crates. The
# binary is position independent: the loader writes each slot from a
# base-relative relocation and then makes the slots read-only (RELRO), so each
# call has one possible target. The calls must become direct calls to two
# functions, and nothing after `panic` may survive in the C. Without RELRO the
# program could change the slots, so the calls stay indirect.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --no-relax -z relro -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
# RUN: ld.lld -pie -s --build-id=none --no-relax -z norelro -e entry %t.o -o %t.norelro
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t.norelro -o %t.norelro.ptml
# RUN: %root/bin/revng ptml --plain %t.norelro.ptml | FileCheck --check-prefix=NORELRO %s
#
# NORELRO: segment_{{[0-9]+}}.got
#
# CHECK-NOT: 0x41414141
# CHECK: function_0x[[PANIC:[0-9a-f]+]]_Code_x86_64();
# CHECK-NEXT: function_0x[[WORK:[0-9a-f]+]]_Code_x86_64();
# CHECK-NOT: 0x41414141
# CHECK: _NORETURN {{.*}} function_0x[[PANIC]]_Code_x86_64(void)
# CHECK: function_0x[[WORK]]_Code_x86_64(void) {
# CHECK-NEXT: return 7UL;

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  test %rdi, %rdi
  jne 1f
  call *panic@GOTPCREL(%rip)
  movl $0x41414141, (%rdi)
1:
  call *work@GOTPCREL(%rip)
  ret
  .cfi_endproc
.size entry, .-entry

.type panic, @function
panic:
  .cfi_startproc
  ud2
  .cfi_endproc
.size panic, .-panic

.type work, @function
work:
  .cfi_startproc
  mov $7, %eax
  ret
  .cfi_endproc
.size work, .-work
