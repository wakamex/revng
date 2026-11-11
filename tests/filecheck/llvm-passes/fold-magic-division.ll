;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt -fold-magic-division -verify %s -S -o - | FileCheck %s

; hl-node splits an asset index into its DEX and position with x / 10000,
; which the compiler emits as the high bits of a 128-bit product:
; (x * 3777893186295716171) >> 75. 3777893186295716171 * 10000 exceeds 2^75
; by 432, at most 2^(75 - 64), so the product is x / 10000 for every x. The
; output prints constants in hexadecimal: 10000 is u0x2710.
;
; CHECK-LABEL: define i64 @divide_by_10000(i64 %x)
define i64 @divide_by_10000(i64 %x) {
  %wide = zext i64 %x to i128
  %product = mul i128 %wide, 3777893186295716171
  %shifted = lshr i128 %product, 75
  %result = trunc i128 %shifted to i64
  ; CHECK: [[Q:%[a-zA-Z0-9._]+]] = udiv i64 %x, u0x2710
  ; CHECK-NEXT: ret i64 [[Q]]
  ret i64 %result
}

; The same multiplier with the constant first
;
; CHECK-LABEL: define i64 @divide_by_10000_commuted(i64 %x)
define i64 @divide_by_10000_commuted(i64 %x) {
  %wide = zext i64 %x to i128
  %product = mul i128 3777893186295716171, %wide
  %shifted = lshr i128 %product, 75
  %result = trunc i128 %shifted to i64
  ; CHECK: udiv i64 %x, u0x2710
  ret i64 %result
}

; A multiplier one less is not a reciprocal of 10000 for all x, since
; 3777893186295716170 * 10000 is below 2^75: the product stays
;
; CHECK-LABEL: define i64 @inexact(i64 %x)
define i64 @inexact(i64 %x) {
  %wide = zext i64 %x to i128
  %product = mul i128 %wide, 3777893186295716170
  %shifted = lshr i128 %product, 75
  %result = trunc i128 %shifted to i64
  ; CHECK-NOT: udiv
  ; CHECK: mul i128
  ret i64 %result
}

; A shift by less than the width of x takes bits of the product a division
; does not: the product stays
;
; CHECK-LABEL: define i64 @narrow_shift(i64 %x)
define i64 @narrow_shift(i64 %x) {
  %wide = zext i64 %x to i128
  %product = mul i128 %wide, 3
  %shifted = lshr i128 %product, 1
  %result = trunc i128 %shifted to i64
  ; CHECK-NOT: udiv
  ; CHECK: mul i128
  ret i64 %result
}
