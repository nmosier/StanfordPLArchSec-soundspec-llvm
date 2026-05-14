; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; A memset that covers the full alloca is an initialization op. The subsequent
; load reads bytes that are now definitely-init, so it must NOT be tagged.

declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)

define i8 @memset_full() {
; CHECK-LABEL: @memset_full(
; CHECK:         [[A:%.*]] = alloca [16 x i8], align 1, !unsafe ![[SCOPE:[0-9]+]]
; CHECK-NOT:     !unsafe.init
; CHECK:         call void @llvm.memset{{.*}}(ptr [[A]], i8 0, i64 16, i1 false), !unsafe ![[SCOPE]]
; CHECK:         [[X:%.*]] = load i8, ptr [[A]], align 1{{$}}
; CHECK-NEXT:    ret i8 [[X]]
  %a = alloca [16 x i8], align 1, !unsafe.init !0
  call void @llvm.memset.p0.i64(ptr %a, i8 0, i64 16, i1 false)
  %x = load i8, ptr %a, align 1
  ret i8 %x
}

; CHECK-DAG: ![[SCOPE]] = !{i32 9}
!0 = !{i32 9}
