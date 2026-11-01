//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// A unary operator applied to a cast is applied before the cast instead:
// `~(uint64_t) x` becomes `(uint64_t) ~x`, and `-(uint64_t) x` becomes
// `(uint64_t) -x`. The rewrite used to drop the operator, so `~(T) x` came out
// as `(T) x`; in Rust's float rounding, an index `len + ~i` became `len + i`,
// and the last digit was never rounded up.

// RUN: %root/bin/revng clift-opt %s --post-process-expressions | FileCheck %s

!void = !clift.void

!int64_t = !clift.int<signed 8>
!uint64_t = !clift.int<unsigned 8>

!f = !clift.func<"/model-type/1001" : !void(!int64_t)>

module attributes {clift.module} {
  clift.func @f<!f>(%arg0 : !int64_t) -> !void {
    // CHECK: clift.expr {
    clift.expr {
      // CHECK: [[NOT:%[A-Za-z0-9]+]] = clift.bitnot %arg0 : !int64_t
      // CHECK: [[CAST:%[A-Za-z0-9]+]] = clift.bitcast [[NOT]] : !int64_t -> !uint64_t
      %0 = clift.bitcast %arg0 : !int64_t -> !uint64_t
      %1 = clift.bitnot %0 : !uint64_t
      // CHECK: clift.yield [[CAST]]
      clift.yield %1 : !uint64_t
    }
    // CHECK: clift.expr {
    clift.expr {
      // CHECK: [[NEG:%[A-Za-z0-9]+]] = clift.neg %arg0 : !int64_t
      // CHECK: [[CAST:%[A-Za-z0-9]+]] = clift.bitcast [[NEG]] : !int64_t -> !uint64_t
      %0 = clift.bitcast %arg0 : !int64_t -> !uint64_t
      %1 = clift.neg %0 : !uint64_t
      // CHECK: clift.yield [[CAST]]
      clift.yield %1 : !uint64_t
    }
    // CHECK: }
  }
}
