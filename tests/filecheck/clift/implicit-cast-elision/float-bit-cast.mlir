//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --elide-implicit-casts | FileCheck %s

!void = !clift.void

!float64_t = !clift.float<8>
!int64_t = !clift.int<signed 8>

!f = !clift.func<
  "/type-definition/1001-CABIFunctionDefinition" : !void()
>

// A bit cast between a floating point value and an integer keeps the bits. The
// implicit conversion in C keeps the number, so the cast cannot be elided.

module attributes {clift.module} {
  clift.func @f<!f>() attributes {
    handle = "/function/0x40001001:Code_x86_64"
  } {
    %x = clift.local : !float64_t
    %y = clift.local : !int64_t

    // CHECK: clift.bitcast %{{[0-9a-z]+}} : !float64_t -> !int64_t
    // CHECK-NOT: clift.implicit_cast
    clift.expr {
      %0 = clift.bitcast %x : !float64_t -> !int64_t
      %1 = clift.assign %y, %0 : !int64_t
      clift.yield %1 : !int64_t
    }

    // CHECK: clift.bitcast %{{[0-9a-z]+}} : !int64_t -> !float64_t
    // CHECK-NOT: clift.implicit_cast
    clift.expr {
      %0 = clift.bitcast %y : !int64_t -> !float64_t
      %1 = clift.assign %x, %0 : !float64_t
      clift.yield %1 : !float64_t
    }
  }
}
