; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Single alloca with a single init store followed by a load.
; - !unsafe.init on the alloca is lowered to !unsafe (and !unsafe.init dropped).
; - The init store inherits !unsafe (it writes to uninit bytes).
; - The load is NOT tagged: by the time we reach it, the store has fully
;   initialized the alloca's bytes.

define i32 @basic(i32 %v) {
; CHECK-LABEL: @basic(
; CHECK:         [[A:%.*]] = alloca i32, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         store i32 %v, ptr [[A]], align 4, !unsafe ![[SCOPE]]
; CHECK:         [[X:%.*]] = load i32, ptr [[A]], align 4
; CHECK-NOT:     load i32, ptr [[A]], align 4, !unsafe
; CHECK:         ret i32 [[X]]
  %a = alloca i32, align 4, !unsafe.init !0
  store i32 %v, ptr %a, align 4
  %x = load i32, ptr %a, align 4
  ret i32 %x
}

; CHECK-DAG: ![[SCOPE]] = !{i32 42}
!0 = !{i32 42}
