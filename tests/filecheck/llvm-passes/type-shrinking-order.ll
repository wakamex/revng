;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; Type shrinking must rewrite instructions in program order. If it followed
; the order of a container keyed by instruction address, the order of the new
; instructions, and of the uses of the shared operands, would depend on where
; the allocator had placed the original ones. Run the pass under an allocator
; that hands out decreasing addresses and require the same output.
;
; RUN: clang -shared -fPIC -O2 %S/../Inputs/descending-malloc.c -o %t.so
; RUN: %root/bin/revng opt -S -preserve-ll-uselistorder -type-shrinking %s -o %t.normal.ll
; RUN: env LD_PRELOAD=%t.so %root/bin/revng opt -S -preserve-ll-uselistorder -type-shrinking %s -o %t.descending.ll
; RUN: diff %t.normal.ll %t.descending.ll
; RUN: FileCheck %s < %t.normal.ll

; CHECK-LABEL: define i64 @f
; CHECK: %[[X1:[0-9]+]] = add i8
; CHECK-NEXT: %[[Y1:[0-9]+]] = and i8 %[[X1]], -1
; CHECK: %[[X2:[0-9]+]] = add i8
; CHECK-NEXT: %[[Y2:[0-9]+]] = and i8 %[[X2]], -1
; CHECK: %[[X3:[0-9]+]] = add i8
; CHECK-NEXT: %[[Y3:[0-9]+]] = and i8 %[[X3]], -1
; CHECK: %[[X4:[0-9]+]] = add i8
; CHECK-NEXT: %[[Y4:[0-9]+]] = and i8 %[[X4]], -1
; CHECK: zext i8 %[[Y1]] to i16
; CHECK-NEXT: zext i8 %[[Y2]] to i16
; CHECK: zext i8 %[[Y3]] to i16
; CHECK-NEXT: zext i8 %[[Y4]] to i16
; CHECK-NOT: uselistorder

define i64 @f(i64 %a, i64 %b) {
  %x1 = add i64 %a, %b
  %y1 = and i64 %x1, 255
  %x2 = add i64 %a, %b
  %y2 = and i64 %x2, 255
  %x3 = add i64 %a, %b
  %y3 = and i64 %x3, 255
  %x4 = add i64 %a, %b
  %y4 = and i64 %x4, 255
  %s1 = add i64 %y1, %y2
  %s2 = add i64 %y3, %y4
  %s = add i64 %s1, %s2
  ret i64 %s
}
