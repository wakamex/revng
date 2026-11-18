//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --c-legalization | FileCheck %s

!void = !clift.void

!float64_t = !clift.float<8>
!int64_t = !clift.int<signed 8>
!uint64_t = !clift.int<unsigned 8>

!f = !clift.func<
  "/type-definition/1001-CABIFunctionDefinition" : !void()
>

// A conversion that already yields the integer its semantics ask for needs no
// cast, whatever the type of the floating point value it converts.

module attributes {clift.module} {
  clift.func @f<!f>() attributes {
    handle = "/function/0x40001001:Code_x86_64"
  } {
    %x = clift.local : !float64_t

    // CHECK: clift.ftos %{{[0-9a-z]+}} : !float64_t -> !int64_t
    // CHECK-NOT: clift.bitcast
    clift.expr {
      %0 = clift.ftos %x : !float64_t -> !int64_t
      clift.yield %0 : !int64_t
    }

    // CHECK: clift.ftou %{{[0-9a-z]+}} : !float64_t -> !uint64_t
    // CHECK-NOT: clift.bitcast
    clift.expr {
      %0 = clift.ftou %x : !float64_t -> !uint64_t
      clift.yield %0 : !uint64_t
    }
  }
}
