; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Negative companion to sret-basic: a call that takes the alloca by pointer
; but WITHOUT the `sret` attribute should NOT count as an initialization. The
; alloca remains uninit from the analysis's point of view, so the call gets
; tagged (AA says may-modref) but for the wrong reason — it's an opaque escape,
; not an init. The subsequent load is ALSO tagged because the alloca was never
; provably initialized.
;
; This documents the current behavior: only sret-annotated calls drive the
; init dataflow. If a frontend wants a non-sret out-param to count as init,
; the attribute must be present.

%S = type { i32, i32 }

declare void @opaque(ptr %p)

define i32 @sret_non_sret_call() {
; CHECK-LABEL: @sret_non_sret_call(
; CHECK:         [[A:%.*]] = alloca %S, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         call void @opaque(ptr [[A]]), !unsafe ![[SCOPE]]
; CHECK:         [[X:%.*]] = load i32, ptr [[A]], align 4, !unsafe ![[SCOPE]]
; CHECK-NEXT:    ret i32 [[X]]
  %a = alloca %S, align 4, !unsafe.init !0
  call void @opaque(ptr %a)
  %x = load i32, ptr %a, align 4
  ret i32 %x
}

; CHECK-DAG: ![[SCOPE]] = !{i32 19}
!0 = !{i32 19}
