;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt -S -loop-rewrite-with-canonical-induction-variable -legalize-integer-widths -verify %s | FileCheck %s

; The low five bits of a counter stepping by 32 do not change in the loop. The
; cleanup after the rewrite of the induction variable hoists them out of it as
; 5-bit integers, which C cannot represent. Reduced from an unrolled bit-array
; loop of a Windows program.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; CHECK-LABEL: define void @f
; CHECK-NOT: i5
; CHECK: and i8 %{{.*}}, 31
; CHECK-NOT: i5
define void @f(i8 %start) {
entry:
  br label %loop

loop:
  %counter = phi i8 [ %start, %entry ], [ %next, %loop ]
  %same = or i8 %counter, 0
  %low = and i8 %same, 31
  %wide = zext i8 %low to i32
  store i32 %wide, ptr null, align 4
  %next = add i8 %counter, 32
  br label %loop
}
