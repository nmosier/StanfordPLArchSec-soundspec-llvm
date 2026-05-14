; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Two branches that each fully initialize the alloca. At the join block, the
; intersection of the predecessors' Out sets makes the bytes definitely-init
; on entry, so the load is NOT tagged. Both branch stores ARE tagged (each
; initializes from an uninit-on-entry state in its own block).

define i32 @branch_merge(i1 %c, i32 %x, i32 %y) {
; CHECK-LABEL: @branch_merge(
; CHECK:         [[A:%.*]] = alloca i32, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
entry:
  %a = alloca i32, align 4, !unsafe.init !0
  br i1 %c, label %t, label %f

t:
; CHECK:       t:
; CHECK-NEXT:    store i32 %x, ptr [[A]], align 4, !unsafe ![[SCOPE]]
  store i32 %x, ptr %a, align 4
  br label %join

f:
; CHECK:       f:
; CHECK-NEXT:    store i32 %y, ptr [[A]], align 4, !unsafe ![[SCOPE]]
  store i32 %y, ptr %a, align 4
  br label %join

join:
; CHECK:       join:
; CHECK-NEXT:    [[R:%.*]] = load i32, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    ret i32 [[R]]
  %r = load i32, ptr %a, align 4
  ret i32 %r
}

; CHECK-DAG: ![[SCOPE]] = !{i32 1}
!0 = !{i32 1}
