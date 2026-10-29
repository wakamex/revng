;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt %s -exit-ssa -S -o - | FileCheck %s

declare i1 @c()
declare void @use(i64)

; %p keeps %init on every iteration, and %u is %p or %other. If the two shared
; a local variable, the store of %other in %b would be what %p reads on the next
; iteration, because %p's incoming from %join is %p itself and gets no store.
; They get separate variables: %a copies %p into %u's, and %p's is stored only
; in %entry.

; CHECK-LABEL: define void @f(i64 %init, i64 %other)
; CHECK: entry:
; CHECK: store i64 %init, ptr [[P:%[0-9]+]]
; CHECK: a:
; CHECK-NEXT: store i64 {{%[0-9]+}}, ptr [[U:%[0-9]+]]
; CHECK: b:
; CHECK-NEXT: store i64 %other, ptr [[U]]
; CHECK-NOT: store {{.*}} ptr [[P]]
; CHECK: exit:

define void @f(i64 %init, i64 %other) {
entry:
  br label %head

head:
  %p = phi i64 [ %init, %entry ], [ %p, %join ]
  %k = call i1 @c()
  br i1 %k, label %a, label %b

a:
  br label %join

b:
  br label %join

join:
  %u = phi i64 [ %p, %a ], [ %other, %b ]
  call void @use(i64 %u)
  %e = call i1 @c()
  br i1 %e, label %head, label %exit

exit:
  call void @use(i64 %p)
  ret void
}
