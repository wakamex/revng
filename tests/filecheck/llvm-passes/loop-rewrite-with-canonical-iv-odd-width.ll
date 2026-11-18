;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt -S -loop-rewrite-with-canonical-induction-variable -verify %s | FileCheck %s

; The low five bits of a counter stepping by 32 do not change in the loop, and
; ScalarEvolution writes `x & 31` as `(zext i5 (trunc x to i5))`. Expanding it
; would put 5-bit integers in the function, which C cannot represent, so the
; `and` is left in place. Reduced from an unrolled bit-array loop of a
; Windows program.

define void @f(i8 %start) {
entry:
  br label %loop

loop:
  %counter = phi i8 [ %start, %entry ], [ %next, %loop ]
  %low = and i8 %counter, 31
  %wide = zext i8 %low to i32
  store i32 %wide, ptr null, align 4
  %next = add i8 %counter, 32
  br label %loop
}

; CHECK-LABEL: define void @f
; CHECK-NOT: i5
; CHECK: %low = and i8 %counter, 31
; CHECK-NOT: i5
