//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --refine-types-and-accesses | FileCheck %s

// A local holding a byte pointer that is assigned a pointer to a struct takes
// the struct pointer type, so that accesses through it become field accesses.

!void = !clift.void

!generic8_t = !clift.int<generic 1>
!generic64_t = !clift.int<generic 8>
!int32_t = !clift.int<signed 4>
!int64_t = !clift.int<signed 8>

!generic8_t$ptr = !clift.ptr<8 to !generic8_t>
!int32_t$ptr = !clift.ptr<8 to !int32_t>
!int64_t$ptr = !clift.ptr<8 to !int64_t>
!generic64_t$ptr = !clift.ptr<8 to !generic64_t>
!void$ptr = !clift.ptr<8 to !void>

!s = !clift.struct<
  "1" : size(16) {
    "" : offset(0) !int64_t,
    "" : offset(8) !int32_t
  }
>
!s$ptr = !clift.ptr<8 to !s>

!f = !clift.func<"/model-type/1001" : !int32_t(!s$ptr)>
!g = !clift.func<"/model-type/1002" : !int64_t(!s$ptr)>

module attributes {clift.module} {
  // CHECK-LABEL: clift.func @f
  clift.func @f<!f>(%arg0 : !s$ptr) -> !int32_t {
    // CHECK: [[LOCAL:%[0-9]+]] = clift.local : !clift.ptr<8 to !_1_>
    %0 = clift.local : !generic8_t$ptr

    clift.expr {
      %1 = clift.bitcast %arg0 : !s$ptr -> !generic8_t$ptr
      %2 = clift.assign %0, %1 : !generic8_t$ptr
      clift.yield %2 : !generic8_t$ptr
    }

    // CHECK: clift.return {
    clift.return {
      %3 = clift.imm 8 : !generic64_t
      %4 = clift.ptr_add %0, %3 : (!generic8_t$ptr, !generic64_t)
      %5 = clift.bitcast %4 : !generic8_t$ptr -> !int32_t$ptr
      %6 = clift.indirection %5 : !int32_t$ptr
      // CHECK: [[ACCESS:%[0-9]+]] = clift.ptr_access<1> [[LOCAL]]
      // CHECK: clift.yield [[ACCESS]] : !int32_t
      clift.yield %6 : !int32_t
    }
  }

  // A cast at a read of the local only says how that read uses it: the type
  // of what is assigned to the local wins.
  // CHECK-LABEL: clift.func @g
  clift.func @g<!g>(%arg0 : !s$ptr) -> !int64_t {
    // CHECK: [[LOCAL:%[0-9]+]] = clift.local : !clift.ptr<8 to !_1_>
    %0 = clift.local : !generic64_t

    // The clifter writes the local through a reinterpretation of its storage
    clift.expr {
      %1 = clift.addressof %0 : !generic64_t$ptr
      %2 = clift.bitcast %1 : !generic64_t$ptr -> !void$ptr
      %3 = clift.bitcast %arg0 : !s$ptr -> !generic64_t
      %4 = clift.bitcast %2 : !void$ptr -> !generic64_t$ptr
      %5 = clift.indirection %4 : !generic64_t$ptr
      %6 = clift.assign %5, %3 : !generic64_t
      clift.yield %6 : !generic64_t
    }

    // CHECK: clift.return {
    clift.return {
      %3 = clift.bitcast %0 : !generic64_t -> !int64_t$ptr
      %4 = clift.indirection %3 : !int64_t$ptr
      // CHECK: [[ACCESS:%[0-9]+]] = clift.ptr_access<0> [[LOCAL]]
      // CHECK: clift.yield [[ACCESS]] : !int64_t
      clift.yield %4 : !int64_t
    }
  }
}
