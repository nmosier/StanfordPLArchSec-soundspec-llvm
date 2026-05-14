; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Only the true branch initializes via sret; the false branch leaves the
; alloca uninitialized. At the join, the load is potentially-uninit and
; must be tagged. The sret call is tagged as an init.

%S = type { i32, i32 }

declare void @make(ptr sret(%S) %out)

define i32 @sret_branch(i1 %c) {
; CHECK-LABEL: @sret_branch(
; CHECK:         [[A:%.*]] = alloca %S, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
entry:
  %a = alloca %S, align 4, !unsafe.init !0
  br i1 %c, label %t, label %f

t:
; CHECK:       t:
; CHECK-NEXT:    call void @make(ptr sret(%S) [[A]]), !unsafe ![[SCOPE]]
  call void @make(ptr sret(%S) %a)
  br label %join

f:
  br label %join

join:
; CHECK:       join:
; CHECK-NEXT:    [[R:%.*]] = load i32, ptr [[A]], align 4, !unsafe ![[SCOPE]]
; CHECK-NEXT:    ret i32 [[R]]
  %r = load i32, ptr %a, align 4
  ret i32 %r
}

; CHECK-DAG: ![[SCOPE]] = !{i32 17}
!0 = !{i32 17}
