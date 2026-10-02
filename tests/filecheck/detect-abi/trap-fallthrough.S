#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `f` returns unless rdi is zero, in which case it executes ud2; int3 padding
# follows it, then `g`, which returns a constant in rax. ud2 does not fall
# through, so the lifted block of ud2 ends after raising the exception, and
# the control-flow graph of `f` ends there: it holds neither the padding nor
# `g`, and `f` returns nothing. Following ud2 into `g` made `f` return rax and
# turned each trap into an indirect tail call.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o - | FileCheck %s --check-prefix=LIFT
# RUN: %root/bin/revng quick artifact emit-cfg --analyses=parse-binary,detect-abi %t | FileCheck %s --check-prefix=CFG
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s --check-prefix=MODEL
#
# LIFT-LABEL: "bb.0x40102d:Code_x86_64":
# LIFT: call void @helper_raise_exception(ptr {{.*}}, i32 6)
# LIFT-NEXT: unreachable
#
# CFG-LABEL: /function/0x401027:Code_x86_64:
# CFG-NOT: 0x401031:Code_x86_64
# CFG: Unreachable
# CFG-NOT: 0x401031:Code_x86_64
# CFG-LABEL: /function/0x401031:Code_x86_64:
#
# MODEL: Name: f
# MODEL-NEXT: Prototype:
# MODEL-NEXT: Kind: DefinedType
# MODEL-NEXT: Definition: "/TypeDefinitions/[[F:[0-9]+]]-RawFunctionDefinition"
# MODEL-NOT: CallSitePrototypes:
# MODEL: Name: g
# MODEL: - ID: [[F]]
# MODEL-NEXT: Kind: RawFunctionDefinition
# MODEL-NEXT: Architecture: x86_64
# MODEL-NEXT: Arguments:
# MODEL-NEXT: - Location: rdi_x86_64
# MODEL-NOT: ReturnValues:
# MODEL: FinalStackOffset: 8

.text
.globl entry
.type entry, @function
entry:
  call f
  call g
  ret
.size entry, .-entry

.globl f
.type f, @function
f:
  test %rdi, %rdi
  je 1f
  ret
1:
  ud2
.size f, .-f
  int3
  int3

.globl g
.type g, @function
g:
  movabs $0x1122334455667788, %rax
  ret
.size g, .-g
.section .note.GNU-stack,"",@progbits
