#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `outer` decodes `movabs; call callee; ret`. `inner` starts inside the movabs
# immediate and decodes `cmp; out; nop...`, so its `out`, which ends with an
# indirect branch, lies within the address range of `outer` without being one
# of its instructions. The indirect branch belongs to `inner`. Attributing it
# to `outer` by address range alone turns the call there into an indirect call
# site as well, and detect-abi aborts.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,-T,%S/overlapping-indirect-branch.ld %s -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: Name: entry
# CHECK: CallSitePrototypes:
# CHECK-NEXT: - CallerBlockAddress: "0x401023:Code_x86_64"
# CHECK-NOT: CallerBlockAddress:
# CHECK: Name: callee

.text
.globl entry
.type entry, @function
entry:
  test %rdi, %rdi
  jne inner
outer:
  .byte 0x48, 0xb8
inner:
  .byte 0x38, 0xe8, 0x4f, 0xe6, 0xfc, 0x90, 0x90, 0x90
  call callee
  ret
.size entry, .-entry

.globl callee
.type callee, @function
callee:
  ret
.size callee, .-callee
.section .note.GNU-stack,"",@progbits
