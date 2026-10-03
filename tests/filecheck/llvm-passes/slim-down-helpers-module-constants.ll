;
; This file is distributed under the MIT License. See LICENSE.md for details.
;
; Check that `slim-down-helpers-module` keeps the initializer of the constants
; an inline helper uses whose copies are interchangeable: the `linkonce_odr`
; message of an abort that `fix-helpers` emits and an `unnamed_addr` string.
; Other globals still lose their initializer, and the globals no inline helper
; uses are removed.
;
; RUN: %root/bin/revng opt -slim-down-helpers-module %s -S -o - | FileCheck %s

; CHECK-DAG: @message = linkonce_odr constant [6 x i8] c"hello\00"
@message = linkonce_odr constant [6 x i8] c"hello\00"

; CHECK-DAG: @file_name = private unnamed_addr constant [7 x i8] c"file.c\00"
@file_name = private unnamed_addr constant [7 x i8] c"file.c\00"

; CHECK-DAG: @table = external constant [2 x i8]
@table = constant [2 x i8] c"\01\02"

; CHECK-DAG: @state = external {{.*}}global [4 x i8]
@state = internal global [4 x i8] c"\00\01\02\03"

; CHECK-NOT: @unused_constant
@unused_constant = private unnamed_addr constant [2 x i8] c"ab"

declare void @revng_abort(ptr)

declare void @report(ptr)

define i8 @helper_reads_constants(i64 %index) section "revng_inline" {
entry:
  %is_valid = icmp ult i64 %index, 2
  br i1 %is_valid, label %valid, label %invalid
valid:
  call void @report(ptr @file_name)
  %entry_address = getelementptr [2 x i8], ptr @table, i64 0, i64 %index
  %entry_value = load i8, ptr %entry_address
  %state_address = getelementptr [4 x i8], ptr @state, i64 0, i64 %index
  %state_value = load i8, ptr %state_address
  %sum = add i8 %entry_value, %state_value
  ret i8 %sum
invalid:
  call void @revng_abort(ptr @message)
  unreachable
}
