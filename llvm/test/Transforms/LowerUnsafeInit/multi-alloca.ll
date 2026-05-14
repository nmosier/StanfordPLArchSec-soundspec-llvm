; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; Two allocas in the same function: only the one carrying !unsafe.init should
; be touched. The other alloca and its store/load must remain unchanged.

define i32 @two_allocas(i32 %x, i32 %y) {
; CHECK-LABEL: @two_allocas(
; CHECK:         [[A:%.*]] = alloca i32, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NEXT:    [[B:%.*]] = alloca i32, align 4{{$}}
; CHECK-NOT:     !unsafe.init
; CHECK:         store i32 %x, ptr [[A]], align 4, !unsafe ![[SCOPE]]
; The store to the untagged alloca must remain untagged.
; CHECK-NEXT:    store i32 %y, ptr [[B]], align 4{{$}}
; CHECK-NEXT:    [[P:%.*]] = load i32, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    [[Q:%.*]] = load i32, ptr [[B]], align 4{{$}}
  %a = alloca i32, align 4, !unsafe.init !0
  %b = alloca i32, align 4
  store i32 %x, ptr %a, align 4
  store i32 %y, ptr %b, align 4
  %p = load i32, ptr %a, align 4
  %q = load i32, ptr %b, align 4
  %r = add i32 %p, %q
  ret i32 %r
}

; CHECK-DAG: ![[SCOPE]] = !{i32 5}
!0 = !{i32 5}
