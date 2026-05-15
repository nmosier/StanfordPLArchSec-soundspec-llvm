; RUN: opt -passes=lower-unsafe-init -S < %s | FileCheck %s

; A struct {i8, i32} has 3 bytes of internal padding between the i8 field at
; offset 0 and the i32 field at offset 4. Field-by-field initialization
; writes bytes [0..1) and [4..8), but the padding bytes [1..4) are never
; written. The dataflow's BV therefore never reaches `all()`, even though
; every *meaningful* byte of the struct is initialized.
;
; Behavior in this isolated test:
; - The two field stores ARE tagged: alloca-derived pointers, BV partial.
; - The opaque call is NOT tagged: the alloca never escapes anywhere in
;   this function (no capturing use), so the flow-sensitive escape check
;   correctly concludes the opaque call cannot reach %a.
; - The load IS tagged: its pointer is alloca-derived, BV is partial
;   (padding bytes never written), AA reports may-modref → tag.
;
; The "padding bytes never written" pathology only inflates tags on real
; workloads where the alloca DOES escape (e.g., its pointer is passed to
; some other function). Then post-init opaque calls also get tagged.

%S = type { i8, i32 }

declare void @opaque()

define i32 @padded_struct_fields(i8 %v1, i32 %v2) {
; CHECK-LABEL: @padded_struct_fields(
; CHECK:         [[A:%.*]] = alloca %S, align 4, !unsafe ![[SCOPE:[0-9]+]]
; CHECK:         [[P1:%.*]] = getelementptr inbounds %S, ptr [[A]], i32 0, i32 0
; CHECK-NEXT:    store i8 %v1, ptr [[P1]], align 4, !unsafe ![[SCOPE]]
; CHECK:         [[P2:%.*]] = getelementptr inbounds %S, ptr [[A]], i32 0, i32 1
; CHECK-NEXT:    store i32 %v2, ptr [[P2]], align 4, !unsafe ![[SCOPE]]
; Alloca never escapes; opaque call cannot reach it — NOT tagged.
; CHECK-NEXT:    call void @opaque(){{$}}
; The load's pointer IS alloca-derived; BV is partial (padding) → tagged.
; CHECK-NEXT:    [[R:%.*]] = load i32, ptr [[P2]], align 4, !unsafe ![[SCOPE]]
  %a = alloca %S, align 4, !unsafe.init !0
  %p1 = getelementptr inbounds %S, ptr %a, i32 0, i32 0
  store i8 %v1, ptr %p1, align 4
  %p2 = getelementptr inbounds %S, ptr %a, i32 0, i32 1
  store i32 %v2, ptr %p2, align 4
  call void @opaque()
  %r = load i32, ptr %p2, align 4
  ret i32 %r
}

; CHECK-DAG: ![[SCOPE]] = !{i32 31}
!0 = !{i32 31}
