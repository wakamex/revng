#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `site` is a C++ virtual call, `return object->method(object, 7)`: it loads
# the vtable through its own argument `rdi`, passes `rdi` on unchanged, sets
# `rsi`, and returns what the method returns in `rax`. The callee is unknown,
# so the call site's prototype must come from `site` itself: `rdi` reaches the
# call holding the value `site` received, and `site` returns `rax`, which only
# the call writes. A prototype without them makes the lifted call drop the
# object pointer and `site` return the vtable pointer it loaded into `rax`.
#
# `forward` makes the same point for a direct call: `entry` reads what
# `forward` returns, and `forward` returns what `site` returns, so `site`
# returns `rax` too, though nothing in `forward` reads it.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/indirect-call-site-prototype.ld %s -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Name: site
# CHECK-NEXT: Prototype:
# CHECK-NEXT: Kind: DefinedType
# CHECK-NEXT: Definition: "/TypeDefinitions/[[SITE:[0-9]+]]-RawFunctionDefinition"
# CHECK-NEXT: CallSitePrototypes:
# CHECK-NEXT: - CallerBlockAddress: "0x401029:Code_x86_64"
# CHECK-NEXT: Prototype:
# CHECK-NEXT: Kind: DefinedType
# CHECK-NEXT: Definition: "/TypeDefinitions/[[CALL:[0-9]+]]-RawFunctionDefinition"
#
# CHECK: - ID: [[SITE]]{{$}}
# CHECK-NEXT: Kind: RawFunctionDefinition
# CHECK-NEXT: Architecture: x86_64
# CHECK-NEXT: Arguments:
# CHECK-NEXT: - Location: rdi_x86_64
# CHECK-NOT: Location:
# CHECK: ReturnValues:
# CHECK-NEXT: - Location: rax_x86_64
# CHECK-NOT: Location:
# CHECK: PreservedRegisters:
#
# CHECK: - ID: [[CALL]]{{$}}
# CHECK-NEXT: Kind: RawFunctionDefinition
# CHECK-NEXT: Architecture: x86_64
# CHECK-NEXT: Arguments:
# CHECK-NEXT: - Location: rsi_x86_64
# CHECK-NOT: Location:
# CHECK: - Location: rdi_x86_64
# CHECK-NOT: Location:
# CHECK: ReturnValues:
# CHECK-NEXT: - Location: rax_x86_64
# CHECK-NOT: Location:
# CHECK: PreservedRegisters:

.text
.globl entry
.type entry, @function
entry:
  mov $object, %edi
  call forward
  add $1, %rax
  ret
.size entry, .-entry

.globl forward
.type forward, @function
forward:
  sub $8, %rsp
  call site
  add $8, %rsp
  ret
.size forward, .-forward

.globl site
.type site, @function
site:
  sub $8, %rsp
  mov (%rdi), %rax
  mov $7, %esi
  call *(%rax)
  add $8, %rsp
  ret
.size site, .-site

.globl method
.type method, @function
method:
  lea (%rsi,%rsi), %rax
  add (%rdi), %rax
  ret
.size method, .-method

.data
.globl object
object:
  .quad vtable
vtable:
  .quad method
.section .note.GNU-stack,"",@progbits
