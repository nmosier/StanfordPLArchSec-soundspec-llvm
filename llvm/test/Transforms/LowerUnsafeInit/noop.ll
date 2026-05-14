; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; An alloca without !unsafe.init must be untouched by this pass.
; Sanity: no metadata is introduced or removed anywhere.

define i32 @noop(i32 %v) {
; CHECK-LABEL: @noop(
; CHECK:         [[A:%.*]] = alloca i32, align 4{{$}}
; CHECK-NOT:     !unsafe
; CHECK:         store i32 %v, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    [[X:%.*]] = load i32, ptr [[A]], align 4{{$}}
; CHECK-NEXT:    ret i32 [[X]]
  %a = alloca i32, align 4
  store i32 %v, ptr %a, align 4
  %x = load i32, ptr %a, align 4
  ret i32 %x
}
