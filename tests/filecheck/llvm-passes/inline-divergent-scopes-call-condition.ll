;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt %s -inline-divergent-scopes -S -o - | FileCheck %s

declare !revng.tags !0 void @scope_closer(ptr)
declare !revng.tags !1 void @goto_block()
declare i32 @kind()
!0 = !{!"marker", !"scope-closer"}
!1 = !{!"marker", !"goto-block"}

; The switch in block_c is copied into block_c_ids, which tests its condition
; again. The condition calls @kind, so it is assigned to a variable first and
; both switches test the value read from it: the C calls @kind once.

define void @g(i1 noundef %a) {

block_a:
  br i1 %a, label %block_b, label %block_c

block_c:
  %k = call i32 @kind()
  %c = add i32 %k, 1
  switch i32 %c, label %block_b [
    i32 0, label %block_e
    i32 1, label %block_f
  ]

block_b:
  ret void

block_e:
  ret void

block_f:
  ret void
}

; CHECK-LABEL: define void @g(i1 noundef %a)
; CHECK: [[VARIABLE:%[0-9]+]] = alloca i32
; CHECK: block_c:
; CHECK-NEXT: %k = call i32 @kind()
; CHECK-NEXT: %c = add i32 %k, 1
; CHECK-NEXT: store i32 %c, ptr [[VARIABLE]]
; CHECK-NEXT: [[FIRST:%[0-9]+]] = load i32, ptr [[VARIABLE]]
; CHECK-NEXT: switch i32 [[FIRST]]
; CHECK: block_c_ids:
; CHECK-NEXT: switch i32 [[FIRST]]
