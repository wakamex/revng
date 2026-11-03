#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `display` ends in a tail call to `common`, which returns: `display` returns
# too, with `common`'s result. A function whose only exit is a direct tail call
# used to be noreturn, since only returns and indirect tail calls counted as
# ways out, and its callers dropped what followed the call: Rust's float
# `Display`, which tail-calls the shared formatting routine, crashed its
# callers.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIE %s -o %t.o
# RUN: ld.lld -pie -s --build-id=none --eh-frame-hdr -e entry %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t [[COMMON:function_0x[0-9a-f]+_Code_x86_64]](generic64_t argument_0) {
# CHECK-NEXT: return argument_0 + 1UL;
# CHECK-NOT: _NORETURN
# CHECK: generic64_t {{function_0x[0-9a-f]+_Code_x86_64}}({{.*}} *argument_0) {
# CHECK-NEXT: generic64_t [[RESULT:var_[0-9]+]] = [[COMMON]](argument_0->{{[a-z_0-9]+}});
# CHECK-NEXT: return [[RESULT]];

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  ret
  .cfi_endproc
.size entry, .-entry

.p2align 4, 0xcc
.type common, @function
common:
  .cfi_startproc
  lea 1(%rdi), %rax
  ret
  .cfi_endproc
.size common, .-common

.p2align 4, 0xcc
.type display, @function
display:
  .cfi_startproc
  mov (%rdi), %rdi
  jmp common
  .cfi_endproc
.size display, .-display

.section .data.rel.ro,"aw"
.quad display
