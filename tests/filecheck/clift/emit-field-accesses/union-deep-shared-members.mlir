//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --emit-field-accesses --canonicalize | FileCheck %s

// Each union holds two of the next by value, and the last an int32_t: every
// traversal to the int32_t goes through all 30 unions, along one of 2^30
// paths. The access takes the first field at each level.

!void = !clift.void
!generic64_t = !clift.int<generic 8>
!int32_t = !clift.int<signed 4>
!int32_t$ptr = !clift.ptr<8 to !int32_t>

!f = !clift.func<
  "1000" as "f" : !void()
>

!u30 = !clift.union<
  "30" : {
    "" : !int32_t
  }
>

!u29 = !clift.union<
  "29" : {
    "" : !u30,
    "" : !u30
  }
>

!u28 = !clift.union<
  "28" : {
    "" : !u29,
    "" : !u29
  }
>

!u27 = !clift.union<
  "27" : {
    "" : !u28,
    "" : !u28
  }
>

!u26 = !clift.union<
  "26" : {
    "" : !u27,
    "" : !u27
  }
>

!u25 = !clift.union<
  "25" : {
    "" : !u26,
    "" : !u26
  }
>

!u24 = !clift.union<
  "24" : {
    "" : !u25,
    "" : !u25
  }
>

!u23 = !clift.union<
  "23" : {
    "" : !u24,
    "" : !u24
  }
>

!u22 = !clift.union<
  "22" : {
    "" : !u23,
    "" : !u23
  }
>

!u21 = !clift.union<
  "21" : {
    "" : !u22,
    "" : !u22
  }
>

!u20 = !clift.union<
  "20" : {
    "" : !u21,
    "" : !u21
  }
>

!u19 = !clift.union<
  "19" : {
    "" : !u20,
    "" : !u20
  }
>

!u18 = !clift.union<
  "18" : {
    "" : !u19,
    "" : !u19
  }
>

!u17 = !clift.union<
  "17" : {
    "" : !u18,
    "" : !u18
  }
>

!u16 = !clift.union<
  "16" : {
    "" : !u17,
    "" : !u17
  }
>

!u15 = !clift.union<
  "15" : {
    "" : !u16,
    "" : !u16
  }
>

!u14 = !clift.union<
  "14" : {
    "" : !u15,
    "" : !u15
  }
>

!u13 = !clift.union<
  "13" : {
    "" : !u14,
    "" : !u14
  }
>

!u12 = !clift.union<
  "12" : {
    "" : !u13,
    "" : !u13
  }
>

!u11 = !clift.union<
  "11" : {
    "" : !u12,
    "" : !u12
  }
>

!u10 = !clift.union<
  "10" : {
    "" : !u11,
    "" : !u11
  }
>

!u9 = !clift.union<
  "9" : {
    "" : !u10,
    "" : !u10
  }
>

!u8 = !clift.union<
  "8" : {
    "" : !u9,
    "" : !u9
  }
>

!u7 = !clift.union<
  "7" : {
    "" : !u8,
    "" : !u8
  }
>

!u6 = !clift.union<
  "6" : {
    "" : !u7,
    "" : !u7
  }
>

!u5 = !clift.union<
  "5" : {
    "" : !u6,
    "" : !u6
  }
>

!u4 = !clift.union<
  "4" : {
    "" : !u5,
    "" : !u5
  }
>

!u3 = !clift.union<
  "3" : {
    "" : !u4,
    "" : !u4
  }
>

!u2 = !clift.union<
  "2" : {
    "" : !u3,
    "" : !u3
  }
>

!u1 = !clift.union<
  "1" : {
    "" : !u2,
    "" : !u2
  }
>

module attributes {clift.module} {
  clift.func @f<!f>() {
    %0 = clift.local : !u1
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !u1>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !u1> -> !clift.ptr<8 to !void>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !void> -> !generic64_t
      %4 = clift.imm 0 : !generic64_t
      %5 = clift.add %3, %4 : !generic64_t
      %6 = clift.bitcast %5 : !generic64_t -> !clift.ptr<8 to !int32_t>
      clift.yield %6 : !int32_t$ptr
    }
  }

  // CHECK-LABEL: clift.func @f<!f>
  // CHECK: clift.ptr_access<0>
  // CHECK-COUNT-29: clift.access<0>
  // CHECK: clift.yield {{%[0-9]+}} : !clift.ptr<8 to !int32_t>
}
