; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; A constructor-style sret call must be recognized as a full initialization
; of the alloca. The call itself is tagged (it writes uninit bytes), and the
; subsequent load reads bytes that are now definitely-init — so the load is
; NOT tagged.

%S = type { i32, i32 }

declare void @make(ptr sret(%S) %out)

define i32 @sret_basic() {
; CHECK-LABEL: @sret_basic(
; CHECK:         [[A:%.*]] = alloca %S, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         call void @make(ptr sret(%S) [[A]]), !unsafe ![[SCOPE]]
; CHECK:         [[X:%.*]] = load i32, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    ret i32 [[X]]
  %a = alloca %S, align 4, !unsafe.init !0
  call void @make(ptr sret(%S) %a)
  %x = load i32, ptr %a, align 4
  ret i32 %x
}

; CHECK-DAG: ![[SCOPE]] = !{i32 11}
!0 = !{i32 11}
