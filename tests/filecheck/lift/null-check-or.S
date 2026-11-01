#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` returns 2 when its first argument is a pointer to a value at least
# the value its second argument points to, and otherwise returns 2 also when
# `partition` returns 0. The C tests the pointer and the comparison together:
# the code for "not equal" runs when the pointer is null or the comparison
# fails, `!p || p->x < q->x`. Merging nested ifs wrote `&&` there when the
# inner if was in the outer else, which dereferences the pointer only when it
# is null; Rust's stable quicksort tests its `Option<&T>` ancestor pivot this
# way, and its C sorted wrongly from 33 elements.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: if (!argument_0 || argument_0->{{[a-z_0-9]+}} < argument_1->{{[a-z_0-9]+}}) {
# CHECK-NOT: !argument_0 &&

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rbx
  mov %rdx, %rbx
  test %rdi, %rdi
  je 1f
  mov (%rdi), %rax
  cmp (%rsi), %rax
  jae 2f
1:
  mov %rbx, %rdi
  call partition
  test %rax, %rax
  jne 3f
2:
  mov $2, %eax
  pop %rbx
  ret
3:
  mov $1, %eax
  pop %rbx
  ret
  .cfi_endproc
.size entry, .-entry

.type partition, @function
partition:
  .cfi_startproc
  lea -7(%rdi), %rax
  ret
  .cfi_endproc
.size partition, .-partition
