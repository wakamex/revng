#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `drop` frees its argument by jumping to `free`'s PLT entry, as compilers
# emit a call to an imported function in tail position, and `entry` passes it
# what `malloc` returned. The PLT entry is inlined into `drop`, which then
# ends with a jump through `free`'s GOT slot: a call to an imported function,
# whose prototype tells what it reads. `drop` takes its argument and passes
# it to `free`. It used to take nothing and call `free(undef(...))`: the call
# was treated as one to an unknown callee, whose arguments are guessed from
# the registers written before it, and `drop` writes none.
#
# Here `free` has rev.ng's default prototype, which reads every general
# purpose argument register. The library imports `free` and `malloc` from a
# stub library it names and finds through its run path, since rev.ng imports
# only undefined symbols it finds in the libraries a binary names.
#
# RUN: rm -rf %t.d && mkdir -p %t.d
# RUN: printf '.text\n.globl free\n.type free,@function\nfree: ret\n.globl malloc\n.type malloc,@function\nmalloc: ret\n' > %t.d/dep.s
# RUN: clang --target=x86_64-unknown-linux-gnu -c %t.d/dep.s -o %t.d/dep.o
# RUN: ld.lld -shared -soname libdep.so %t.d/dep.o -o %t.d/libdep.so
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared -s --build-id=none %t.o -L%t.d -ldep -rpath %t.d -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64(generic64_t argument_0{{.*}}) {
# CHECK-NEXT: free(argument_0
# CHECK: entry(
# CHECK-NEXT: malloc(
# CHECK-NEXT: function_0x{{[0-9a-f]+}}_Code_x86_64({{.+}});

.text
.type drop, @function
drop:
  .cfi_startproc
  jmp free@PLT
  .cfi_endproc
.size drop, .-drop

.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  mov %rdi, %rbx
  call malloc@PLT
  mov %rax, %rdi
  call drop
  mov %rbx, %rax
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry

