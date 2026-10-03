;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; helpers-module-to-declarations must list the helpers in @helpers_list in
; module order. It used to collect them in a set of function pointers, so the
; order followed the addresses the functions had been allocated at. Run the pass
; normally and under an allocator that hands out decreasing addresses and
; require module order from both.
;
; RUN: clang -shared -fPIC -O2 %S/../Inputs/descending-malloc.c -o %t.so
; RUN: %root/bin/revng opt -S -helpers-module-to-declarations %s -o %t.normal.ll
; RUN: env LD_PRELOAD=%t.so %root/bin/revng opt -S -helpers-module-to-declarations %s -o %t.descending.ll
; RUN: FileCheck %s < %t.normal.ll
; RUN: FileCheck %s < %t.descending.ll

; CHECK: @helpers_list = constant [8 x ptr] [ptr @helper_a, ptr @helper_b, ptr @helper_c, ptr @helper_d, ptr @helper_e, ptr @helper_f, ptr @helper_g, ptr @helper_h]

define void @helper_a() {
  ret void
}

define void @helper_b() {
  ret void
}

define void @helper_c() {
  ret void
}

define void @helper_d() {
  ret void
}

define void @not_a_helper() {
  ret void
}

define void @helper_e() {
  ret void
}

define void @helper_f() {
  ret void
}

define void @helper_g() {
  ret void
}

define void @helper_h() {
  ret void
}
