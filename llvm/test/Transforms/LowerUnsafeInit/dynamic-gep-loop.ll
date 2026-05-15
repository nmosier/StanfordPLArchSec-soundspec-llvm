; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; `accumulateConstantOffset` only recognizes inbounds GEPs with constant
; indices. A loop-based init `for i in 0..N { a[i] = 0 }` produces a GEP
; with a dynamic SSA index, so `getInitRanges` doesn't recognize it,
; transferInstruction is a no-op, and BV never advances.
;
; Consequences:
; - The store inside the loop body IS tagged: BV is still empty when we
;   visit it, the store's pointer is alloca-derived (via the GEP),
;   `PointerMayBeCapturedBefore` is false (storing-to isn't a capture)
;   so the operand walk fires, finds the alloca-derived pointer, and
;   falls through to AA — which reports may-modref → tag.
;
; - The `@opaque()` call AFTER the loop is NOT tagged: %a never escapes
;   anywhere in this function (the GEP-then-store sequence captures
;   nothing), so the flow-sensitive escape check correctly concludes the
;   opaque call cannot reach %a, regardless of BV's state.
;
; This documents what the pass does in isolation. In real Rust code the
; pattern usually combines with an sret constructor that DOES make %a
; escape — that's where AA pessimism bites and you see large CallBase
; tag counts on real workloads. This test alone does not exhibit that.

declare void @opaque()

define void @loop_init(i32 %n) {
; CHECK-LABEL: @loop_init(
; CHECK:         [[A:%.*]] = alloca [16 x i8], align 1, !unsafe ![[SCOPE:[0-9]+]]
entry:
  %a = alloca [16 x i8], align 1, !unsafe.init !0
  br label %loop

loop:
; CHECK:       loop:
; The store has a dynamic GEP. Transfer doesn't recognize it → BV never
; advances → operand walk fires → AA tags it.
; CHECK:         store i8 0, ptr {{%.*}}, align 1, !unsafe ![[SCOPE]]
  %i = phi i32 [0, %entry], [%i1, %loop]
  %p = getelementptr inbounds [16 x i8], ptr %a, i32 0, i32 %i
  store i8 0, ptr %p, align 1
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, 16
  br i1 %c, label %loop, label %done

done:
; CHECK:       done:
; The alloca never escapes (storing TO a pointer is not a capture), so
; the opaque call cannot reach %a — NOT tagged.
; CHECK-NEXT:    call void @opaque(){{$}}
; CHECK-NEXT:    ret void
  call void @opaque()
  ret void
}

; CHECK-DAG: ![[SCOPE]] = !{i32 37}
!0 = !{i32 37}
