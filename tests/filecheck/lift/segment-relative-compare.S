#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` jumps through a table of 32-bit offsets from the table, indexed by 0
# or 3, whose entries name two targets. The lifted dispatch compares the
# target, the table's segment address plus the entry, with one target's
# original address, which holds only where the segment sits at its original
# address. The C compares the entry with that target's offset from the table
# instead. Rust's BTreeMap range search dispatches on its search bound this
# way, and its C found the wrong leaf for most lower bounds. The index is an
# integer offset added to the table's address: taking it as the pointer
# added the address to a null pointer when the index was 0, which compilers
# may assume does not happen.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK-NOT: (generic8_t *) 12UL
# CHECK: return *(generic32_t *) {{.+}} == 4196U ? argument_0 + 1UL : argument_0 << 1UL;
# CHECK-NOT: 4199025UL
# CHECK-NOT: (generic8_t *) 12UL

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  xor %eax, %eax
  mov $3, %ecx
  cmp %rsi, %rdi
  cmove %ecx, %eax
  lea table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  add %rcx, %rax
  jmp *%rax
case0:
  lea 1(%rdi), %rax
  ret
case3:
  lea (%rdi,%rdi), %rax
  ret
  .cfi_endproc
.size entry, .-entry

.section .rodata
table:
  .long case0 - table
  .long case0 - table
  .long case0 - table
  .long case3 - table
