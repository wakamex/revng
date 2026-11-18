;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt -S -legalize-integer-widths -verify %s | FileCheck %s

; CHECK-LABEL: define i8 @low_bits
; CHECK-NEXT: [[TRUNC:%.*]] = trunc i64 %x to i8
; CHECK-NEXT: [[ADD:%.*]] = add i8 [[TRUNC]], 31
; CHECK-NEXT: [[MASK:%.*]] = and i8 [[ADD]], 31
; CHECK-NEXT: ret i8 [[MASK]]
define i8 @low_bits(i64 %x) {
  %low = trunc i64 %x to i5
  %minus = add i5 %low, -1
  %wide = zext i5 %minus to i8
  ret i8 %wide
}

; CHECK-LABEL: define i32 @signed_value
; CHECK-NEXT: [[TRUNC:%.*]] = trunc i64 %x to i16
; CHECK-NEXT: [[SHL:%.*]] = shl i16 [[TRUNC]], 4
; CHECK-NEXT: [[ASHR:%.*]] = ashr i16 [[SHL]], 4
; CHECK-NEXT: [[SEXT:%.*]] = sext i16 [[ASHR]] to i32
; CHECK-NEXT: ret i32 [[SEXT]]
define i32 @signed_value(i64 %x) {
  %low = trunc i64 %x to i12
  %wide = sext i12 %low to i32
  ret i32 %wide
}

; CHECK-LABEL: define i1 @compare
; CHECK-NEXT: [[TRUNC:%.*]] = trunc i64 %x to i8
; CHECK-NEXT: [[MASK:%.*]] = and i8 [[TRUNC]], 31
; CHECK-NEXT: [[CMP:%.*]] = icmp ult i8 [[MASK]], 3
; CHECK-NEXT: ret i1 [[CMP]]
define i1 @compare(i64 %x) {
  %low = trunc i64 %x to i5
  %result = icmp ult i5 %low, 3
  ret i1 %result
}

; CHECK-LABEL: define i8 @shift_right
; CHECK-NEXT: [[TRUNC:%.*]] = trunc i64 %x to i8
; CHECK-NEXT: [[MASK:%.*]] = and i8 [[TRUNC]], 31
; CHECK-NEXT: [[SHR:%.*]] = lshr i8 [[MASK]], 1
; CHECK-NEXT: [[RESULT:%.*]] = and i8 [[SHR]], 31
; CHECK-NEXT: ret i8 [[RESULT]]
define i8 @shift_right(i64 %x) {
  %low = trunc i64 %x to i5
  %half = lshr i5 %low, 1
  %wide = zext i5 %half to i8
  ret i8 %wide
}

; CHECK-LABEL: define i8 @loop
; CHECK: phi i8 [ 1, %entry ], [ [[NEXT:%.*]], %loop ]
; CHECK: [[NEXT]] = add i8
; CHECK-NOT: i5
define i8 @loop(i1 %c) {
entry:
  br label %loop

loop:
  %value = phi i5 [ 1, %entry ], [ %next, %loop ]
  %next = add i5 %value, 2
  br i1 %c, label %loop, label %exit

exit:
  %wide = zext i5 %next to i8
  ret i8 %wide
}

; A value this pass cannot rewrite stays as it is
; CHECK-LABEL: define void @unsupported
; CHECK-NEXT: store i5 %x, ptr %p
define void @unsupported(i5 %x, ptr %p) {
  store i5 %x, ptr %p
  ret void
}
