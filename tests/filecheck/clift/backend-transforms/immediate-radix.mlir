//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --deduce-immediate-radices | FileCheck %s

!void = !clift.void
!int32_t = !clift.int<signed 4>
!uint8_t = !clift.int<unsigned 1>
!uint64_t = !clift.int<unsigned 8>
!uint8_t$ptr = !clift.ptr<8 to !uint8_t>

!f = !clift.func<
  "/type-definition/1001-CABIFunctionDefinition" : !void()
>

module attributes {clift.module} {
  clift.func @f<!f>() {
    // CHECK: clift.expr {
    clift.expr {
      // COM: 123456789 = 0x75BCD15
      // CHECK: %0 = clift.imm 123456789 : !int32_t
      %0 = clift.imm 123456789 : !int32_t
      // CHECK: clift.yield %0 : !int32_t
      clift.yield %0 : !int32_t
    // CHECK: }
    }

    // CHECK: clift.expr {
    clift.expr {
      // COM: 11185083 = 0xAAABBB
      // CHECK: %0 = clift.imm 11185083 {clift.radix = 16 : ui32} : !int32_t
      %0 = clift.imm 11185083 : !int32_t
      // CHECK: clift.yield %0 : !int32_t
      clift.yield %0 : !int32_t
    // CHECK: }
    }

    // CHECK: clift.expr {
    clift.expr {
      // CHECK: %0 = clift.imm 1 {clift.radix = 16 : ui32} : !int32_t
      %0 = clift.imm 1 : !int32_t
      // CHECK: %1 = clift.imm 1000 {clift.radix = 16 : ui32} : !int32_t
      %1 = clift.imm 1000 : !int32_t
      // CHECK: %2 = clift.bitor %0, %1 : !int32_t
      %2 = clift.bitor %0, %1 : !int32_t
      // CHECK: clift.yield %2 : !int32_t
      clift.yield %2 : !int32_t
    // CHECK: }
    }
  }

  clift.func @g<!f>() {
    %p = clift.local : !uint8_t$ptr

    // An offset added to a pointer is in hexadecimal
    // CHECK: clift.expr {
    clift.expr {
      // COM: 9648 = 0x25B0
      // CHECK: {{%[0-9]+}} = clift.imm 9648 {clift.radix = 16 : ui32} : !uint64_t
      %1 = clift.imm 9648 : !uint64_t
      %2 = clift.ptr_add %p, %1 : (!uint8_t$ptr, !uint64_t)
      clift.yield %2 : !uint8_t$ptr
    }

    // A small one is not
    // CHECK: clift.expr {
    clift.expr {
      // CHECK: {{%[0-9]+}} = clift.imm 8 : !uint64_t
      %1 = clift.imm 8 : !uint64_t
      %2 = clift.ptr_add %p, %1 : (!uint8_t$ptr, !uint64_t)
      clift.yield %2 : !uint8_t$ptr
    }

    // So is one added to a pointer converted to an integer
    // CHECK: clift.expr {
    clift.expr {
      %1 = clift.bitcast %p : !uint8_t$ptr -> !uint64_t
      // COM: 23504 = 0x5BD0
      // CHECK: {{%[0-9]+}} = clift.imm 23504 {clift.radix = 16 : ui32} : !uint64_t
      %2 = clift.imm 23504 : !uint64_t
      %3 = clift.add %1, %2 : !uint64_t
      clift.yield %3 : !uint64_t
    }

    // An integer added to another integer keeps its radix
    // CHECK: clift.expr {
    clift.expr {
      // CHECK: {{%[0-9]+}} = clift.imm 23504 : !uint64_t
      %0 = clift.imm 23504 : !uint64_t
      %1 = clift.imm 7 : !uint64_t
      %2 = clift.add %0, %1 : !uint64_t
      clift.yield %2 : !uint64_t
    }
  }
}
