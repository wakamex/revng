#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `pair` returns two values in `rax` and `rdx`, which `entry` reads, unless
# its argument is zero, in which case it calls `release` and returns, so both
# registers are alive after that call. `release` writes neither register. A
# register a caller reads after a call is a return value of the callee only if
# the callee may return a value in it: one it writes and does not read again
# before returning, or one a callee of its own may return. `release` returns
# nothing; it used to return `rax` and `rdx`, which made it a raw function
# returning a structure, as Rust's `__rust_dealloc` did since many of its
# callers call it last on some path.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld --build-id=none -e entry %t.o -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Name: release
# CHECK-NEXT: Prototype:
# CHECK-NEXT: Kind: DefinedType
# CHECK-NEXT: Definition: "/TypeDefinitions/[[RELEASE:[0-9]+]]-RawFunctionDefinition"
#
# CHECK: - ID: [[RELEASE]]{{$}}
# CHECK-NEXT: Kind: RawFunctionDefinition
# CHECK-NEXT: Architecture: x86_64
# CHECK-NEXT: Arguments:
# CHECK-NEXT: - Location: rdi_x86_64
# CHECK-NOT: ReturnValues:
# CHECK: PreservedRegisters:

.text
.globl entry
.type entry, @function
entry:
  call pair
  add %rdx, %rax
  ret
.size entry, .-entry

.globl pair
.type pair, @function
pair:
  test %rdi, %rdi
  jz 1f
  mov %rdi, %rax
  lea 1(%rdi), %rdx
  ret
1:
  push %rax
  call release
  pop %rcx
  ret
.size pair, .-pair

.globl release
.type release, @function
release:
  mov %rdi, sink(%rip)
  ret
.size release, .-release

.bss
sink:
  .quad 0
