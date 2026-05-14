; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; After an sret call fully initializes the alloca, a subsequent store into
; one of its fields is an *overwrite* — those bytes are already init, so the
; store must NOT be tagged. This is the sret analog of overwrite.ll.

%S = type { i32, i32 }

declare void @make(ptr sret(%S) %out)

define void @sret_then_store(i32 %v) {
; CHECK-LABEL: @sret_then_store(
; CHECK:         [[A:%.*]] = alloca %S, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         call void @make(ptr sret(%S) [[A]]), !unsafe ![[SCOPE]]
; The follow-up store overwrites init bytes; must NOT be tagged.
; CHECK:         [[F:%.*]] = getelementptr inbounds %S, ptr [[A]], i32 0, i32 1
; CHECK-NEXT:    store i32 %v, ptr [[F]], align 4{{$}}
; CHECK-NEXT:    ret void
  %a = alloca %S, align 4, !unsafe.init !0
  call void @make(ptr sret(%S) %a)
  %f = getelementptr inbounds %S, ptr %a, i32 0, i32 1
  store i32 %v, ptr %f, align 4
  ret void
}

; CHECK-DAG: ![[SCOPE]] = !{i32 13}
!0 = !{i32 13}
