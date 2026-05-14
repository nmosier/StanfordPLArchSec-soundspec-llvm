; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; A second store that overwrites the same bytes as the first should NOT be
; tagged. The BV must be updated after the first store is marked, so the
; second store sees the bytes as already-initialized.

define void @overwrite() {
; CHECK-LABEL: @overwrite(
; CHECK:         [[A:%.*]] = alloca i32, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         store i32 1, ptr [[A]], align 4, !unsafe ![[SCOPE]]
; The second store overwrites already-initialized bytes; must NOT be tagged.
; CHECK-NEXT:    store i32 2, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    ret void
  %a = alloca i32, align 4, !unsafe.init !0
  store i32 1, ptr %a, align 4
  store i32 2, ptr %a, align 4
  ret void
}

; CHECK-DAG: ![[SCOPE]] = !{i32 3}
!0 = !{i32 3}
