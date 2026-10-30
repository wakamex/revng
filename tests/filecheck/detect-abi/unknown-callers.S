#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `method` is reached only through the pointer in `table`, as a method is
# through a vtable, so no call in the binary shows its callers. It calls a
# function pointer from its object, passing on the registers its caller set
# besides `rdi`, and returns what that call returns in `rax` without reading
# it. Callers the binary shows tell which registers they pass and read; here
# none do, so `method` takes the registers the unknown callee receives
# unchanged and returns the call's result.
#
# `forward` is exported, so callers outside the binary may call it. It calls
# `twice` and returns its result without reading it: `forward` returns what
# `twice` returns.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared -s --build-id=none -z relro %t.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: generic64_t function_0x{{[0-9a-f]+}}_Code_x86_64({{.*}}argument_0, generic64_t argument_1, generic64_t argument_2
# CHECK: [[RESULT:var_[0-9]+]] = {{.*}}(argument_0->{{.*}}, argument_1, argument_2
# CHECK-NEXT: return [[RESULT]];
#
# CHECK: generic64_t forward(generic64_t argument_0) {
# CHECK-NEXT: return function_0x{{[0-9a-f]+}}_Code_x86_64(argument_0);

.text
.type method, @function
method:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  mov 8(%rdi), %rax
  mov (%rdi), %rdi
  call *%rax
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size method, .-method

.section .data.rel.ro,"aw"
.globl table
.type table, @object
table:
  .quad method
.size table, 8

.text
.globl forward
.type forward, @function
forward:
  .cfi_startproc
  push %rax
  .cfi_def_cfa_offset 16
  call twice
  pop %rcx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size forward, .-forward

.type twice, @function
twice:
  .cfi_startproc
  lea (%rdi,%rdi), %rax
  ret
  .cfi_endproc
.size twice, .-twice
