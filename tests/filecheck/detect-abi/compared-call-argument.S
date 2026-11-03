#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `writer` loads a length into rdx, tests it and, when it is not zero, calls
# through a pointer with rdx unchanged, as Rust writes a string slice with
# `if len != 0 { out.write_str(ptr, len) }`. The test leaves the value in the
# register, so the call takes it. A register read before a call used to be
# no argument of it, so the call lost its length: Rust's float formatting
# passed a pointer as the length of its sign and aborted on negative numbers.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: if (argument_2->[[LENGTH:[a-z_0-9.]+]])
# CHECK-NEXT: {{.*}} = (({{.*}} *) argument_1->{{[a-z_0-9.]+}})(argument_0, argument_2->{{[a-z_0-9.]+}}, argument_2->[[LENGTH]]);

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  ret
  .cfi_endproc
.size entry, .-entry

.p2align 4, 0xcc
.type writer, @function
writer:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  push %r15
  .cfi_def_cfa_offset 24
  push %rax
  .cfi_def_cfa_offset 32
  mov %rdx, %r15
  mov %rsi, %rbx
  mov 0x8(%rdx), %rdx
  test %rdx, %rdx
  je 1f
  mov (%r15), %rsi
  call *0x18(%rbx)
1:
  pop %rax
  pop %r15
  pop %rbx
  ret
  .cfi_endproc
.size writer, .-writer

.section .data.rel.ro,"aw"
.quad writer
