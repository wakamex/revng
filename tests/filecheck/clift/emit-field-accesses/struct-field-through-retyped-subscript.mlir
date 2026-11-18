//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --refine-types-and-accesses | FileCheck %s

// A local starts as a byte pointer and gets the type of a pointer to a struct
// only once the field it is loaded from is recognised, after the access through
// it has been rewritten as a subscript of a byte pointer, as a BTreeMap node's
// `len` in a lookup of a Rust map. The address of the subscript, of the
// retyped local cast back to a byte pointer, is then the field's address.

!void = !clift.void
!generic64_t = !clift.int<generic 8>
!generic8_t = !clift.int<generic 1>
!generic16_t = !clift.int<generic 2>
!uint8_t = !clift.int<unsigned 1>
!uint16_t = !clift.int<unsigned 2>
!uint64_t = !clift.int<unsigned 8>
!node = !clift.struct<
  "1" : size(208) {
    "" : offset(0) !clift.ptr<8 to !clift.struct<"1">>,
    "" : offset(8) !clift.array<11 x !uint64_t>,
    "" : offset(96) !uint16_t,
    "" : offset(98) !uint16_t,
    "" : offset(100) !clift.array<11 x !uint8_t>,
    "" : offset(112) !clift.array<12 x !clift.ptr<8 to !clift.struct<"1">>>
  }
>
!node$ptr = !clift.ptr<8 to !node>
!map = !clift.struct<
  "2" : size(24) {
    "" : offset(0) !node$ptr,
    "" : offset(8) !uint64_t,
    "" : offset(16) !uint64_t
  }
>
!holder = !clift.struct<
  "3" : size(24) {
    "" : offset(0) !map
  }
>
!holder$ptr = !clift.ptr<8 to !holder>
!f = !clift.func<"/model-type/1001" : !void(!holder$ptr)>
module attributes {clift.module} {
  clift.func @f<!f>(%arg0 : !holder$ptr) -> !void {
    %0 = clift.local : !clift.ptr<8 to !generic8_t>
    clift.expr {
      %8 = clift.bitcast %arg0 : !holder$ptr -> !clift.ptr<8 to !generic64_t>
      %9 = clift.indirection %8 : !clift.ptr<8 to !generic64_t>
      %1 = clift.bitcast %9 : !generic64_t -> !clift.ptr<8 to !generic8_t>
      %2 = clift.assign %0, %1 : !clift.ptr<8 to !generic8_t>
      clift.yield %2 : !clift.ptr<8 to !generic8_t>
    }
    clift.expr {
      %4 = clift.imm 98 : !generic64_t
      %5 = clift.ptr_add %0, %4 : (!clift.ptr<8 to !generic8_t>, !generic64_t)
      %6 = clift.bitcast %5 : !clift.ptr<8 to !generic8_t> -> !clift.ptr<8 to !generic16_t>
      %7 = clift.indirection %6 : !clift.ptr<8 to !generic16_t>
      clift.yield %7 : !generic16_t
    }
  }
}

// CHECK-LABEL: clift.func @f
// CHECK: %0 = clift.local : !clift.ptr<8 to !_1_>
// CHECK: clift.ptr_access<3> %0 : !clift.ptr<8 to !_1_> -> !uint16_t
// CHECK-NOT: clift.subscript
