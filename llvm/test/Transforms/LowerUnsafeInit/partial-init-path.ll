; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Only the true branch initializes; the false branch leaves the alloca
; uninitialized. The load at the join is potentially-uninit, so it must be
; tagged.

define i32 @partial_init(i1 %c, i32 %v) {
; CHECK-LABEL: @partial_init(
; CHECK:         [[A:%.*]] = alloca i32, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
entry:
  %a = alloca i32, align 4, !unsafe.init !0
  br i1 %c, label %t, label %f

t:
; CHECK:       t:
; CHECK-NEXT:    store i32 %v, ptr [[A]], align 4, !unsafe ![[SCOPE]]
  store i32 %v, ptr %a, align 4
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

; CHECK-DAG: ![[SCOPE]] = !{i32 7}
!0 = !{i32 7}
