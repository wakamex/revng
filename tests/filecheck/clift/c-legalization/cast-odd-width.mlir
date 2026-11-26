//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// C has no integer type of 3 bytes, so casts to and from one become
// intrinsics.

// RUN: %root/bin/revng clift-opt %s --c-legalization | FileCheck %s

!void = !clift.void
!uint24_t = !clift.int<unsigned 3>
!uint64_t = !clift.int<unsigned 8>

!f = !clift.func<
  "/type-definition/1001-CABIFunctionDefinition" : !void(!uint64_t)
>

module attributes {clift.module} {
  clift.func @f<!f>(%arg0 : !uint64_t) attributes {
    handle = "/function/0x40001001:Code_x86_64"
  } {
    clift.expr {
      // CHECK: %0 = clift.truncate %arg0 {clift.intrinsic} : !uint64_t -> !uint24_t
      %0 = clift.truncate %arg0 : !uint64_t -> !uint24_t
      // CHECK: %1 = clift.zext %0 {clift.intrinsic} : !uint24_t -> !uint64_t
      %1 = clift.zext %0 : !uint24_t -> !uint64_t
      clift.yield %1 : !uint64_t
    }
  }
}
