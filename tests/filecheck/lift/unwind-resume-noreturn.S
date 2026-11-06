#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `f` ends as a landing pad does, calling `_Unwind_Resume`, which never
# returns, and the code after it is another function, which `k` reaches by
# a jump. GCC's unwind.h declares `_Unwind_Resume` without noreturn, so
# prototypes taken from headers do not say it, and rev.ng's database does
# not either; the importer marks it noreturn. `f` used to go on into the next
# function's code and return what it loads, and branches to that code were
# into the middle of `f`: hl-node calls `_Unwind_Resume` at the end of 46,422
# landing pads.
#
# The library imports `_Unwind_Resume` from a stub library it names and finds
# through its run path, since rev.ng imports only undefined symbols it finds
# in the libraries a binary names.
#
# RUN: rm -rf %t.d && mkdir -p %t.d
# RUN: printf '.text\n.globl _Unwind_Resume\n.type _Unwind_Resume,@function\n_Unwind_Resume: ret\n' > %t.d/dep.s
# RUN: clang --target=x86_64-unknown-linux-gnu -c %t.d/dep.s -o %t.d/dep.o
# RUN: ld.lld -shared -soname libdep.so %t.d/dep.o -o %t.d/libdep.so
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared -s --build-id=none %t.o -L%t.d -ldep -rpath %t.d -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: _NORETURN _ABI(SystemV_x86_64) void function_0x{{[0-9a-f]+}}_Code_x86_64(
# CHECK-NEXT: _Unwind_Resume(
# CHECK-NEXT: }

.text
.globl entry
.type entry, @function
entry:
  push %rbx
  mov %rdi, %rbx
  call k
  mov %rbx, %rdi
  call f
  pop %rbx
  ret
.size entry, .-entry

.type f, @function
f:
  push %rax
  call _Unwind_Resume@PLT
.size f, .-f

# Right after `f`, reached only by the jump in `k`
.type h, @function
h:
  mov (%rdi), %rax
  ret
.size h, .-h

.type k, @function
k:
  jmp h
.size k, .-k
