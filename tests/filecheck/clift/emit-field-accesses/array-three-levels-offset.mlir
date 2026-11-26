//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --emit-field-accesses --canonicalize | FileCheck %s

!void = !clift.void
!generic64_t = !clift.int<generic 8>
!int32_t = !clift.int<signed 4>
!int32_t$ptr = !clift.ptr<8 to !int32_t>

!f = !clift.func<
  "1000" as "f" : !void()
>

// Three nested arrays, each at a nonzero offset in its parent's element:
// struct T { int32_t pad[2]; struct E1 { int32_t pad; struct E2 { int32_t pad;
// int32_t c[4]; } b[2]; } a[2]; }
// The innermost array is 4 bytes into an element of b, which is 4 bytes into
// an element of a, which starts 8 bytes into T. The offset of each array in its
// parent's element must leave out the offsets of the arrays further out.
!e2 = !clift.struct<
  "1" : size(20) {
    "" : offset(0) !int32_t,
    "" : offset(4) !clift.array<4 x !int32_t>
  }
>

!e1 = !clift.struct<
  "2" : size(44) {
    "" : offset(0) !int32_t,
    "" : offset(4) !clift.array<2 x !e2>
  }
>

!t = !clift.struct<
  "3" : size(96) {
    "" : offset(0) !clift.array<2 x !int32_t>,
    "" : offset(8) !clift.array<2 x !e1>
  }
>

module attributes {clift.module} {
  // 8 + 44 * 1 + 4 + 20 * 1 + 4 + 4 * 1 = 84: t.a[1].b[1].c[1]
  clift.func @second<!f>() {
    %0 = clift.local : !t
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !t>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !t> -> !clift.ptr<8 to !void>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !void> -> !generic64_t
      %4 = clift.imm 84 : !generic64_t
      %5 = clift.add %3, %4 : !generic64_t
      %6 = clift.bitcast %5 : !generic64_t -> !int32_t$ptr
      clift.yield %6 : !int32_t$ptr
    }
  }

  // CHECK-LABEL: clift.func @second<!f>
  // CHECK: clift.ptr_access<1>
  // CHECK: clift.imm 1
  // CHECK: clift.subscript
  // CHECK: clift.access<1>
  // CHECK: clift.imm 1
  // CHECK: clift.subscript
  // CHECK: clift.access<1>
  // CHECK-NOT: clift.indirection
  // CHECK: clift.imm 1
  // CHECK: [[ELEMENT:%[0-9]+]] = clift.subscript
  // CHECK: [[ADDRESS:%[0-9]+]] = clift.addressof [[ELEMENT]]
  // CHECK: clift.yield [[ADDRESS]] : !clift.ptr<8 to !int32_t>

  // 8 + 44 * 1 + 4 + 20 * 1 + 4 + 4 * 3 = 92: t.a[1].b[1].c[3]
  clift.func @last<!f>() {
    %0 = clift.local : !t
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !t>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !t> -> !clift.ptr<8 to !void>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !void> -> !generic64_t
      %4 = clift.imm 92 : !generic64_t
      %5 = clift.add %3, %4 : !generic64_t
      %6 = clift.bitcast %5 : !generic64_t -> !int32_t$ptr
      clift.yield %6 : !int32_t$ptr
    }
  }

  // CHECK-LABEL: clift.func @last<!f>
  // CHECK: clift.ptr_access<1>
  // CHECK: clift.imm 1
  // CHECK: clift.subscript
  // CHECK: clift.access<1>
  // CHECK: clift.imm 1
  // CHECK: clift.subscript
  // CHECK: clift.access<1>
  // CHECK-NOT: clift.indirection
  // CHECK: clift.imm 3
  // CHECK: [[ELEMENT:%[0-9]+]] = clift.subscript
  // CHECK: [[ADDRESS:%[0-9]+]] = clift.addressof [[ELEMENT]]
  // CHECK: clift.yield [[ADDRESS]] : !clift.ptr<8 to !int32_t>
}
