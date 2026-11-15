; A quad over the whole target for tools/air/clipcheck.c, its corners in a constant buffer, with two
; clip distances: x + 0.5, which clips away the quarter left of x = -0.5, and 0.5 - y, which clips
; the quarter above y = 0.5 (VectorKit's raised fills write such an array, RealityKit six).
;
;     struct V { float4 p [[position]]; float clip [[clip_distance]] [2]; };
;     vertex V cv(constant float4 *corner [[buffer(0)]], uint v [[vertex_id]]) {
;         float4 p = corner[v]; return { p, { p.x + 0.5, 0.5 - p.y } }; }
source_filename = "clip"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define <{ <4 x float>, [2 x float] }> @cv(ptr addrspace(2) %0, i32 %1) local_unnamed_addr {
  %w = zext i32 %1 to i64
  %p = getelementptr inbounds <4 x float>, ptr addrspace(2) %0, i64 %w
  %c = load <4 x float>, ptr addrspace(2) %p, align 16
  %x = extractelement <4 x float> %c, i64 0
  %y = extractelement <4 x float> %c, i64 1
  %d0 = fadd float %x, 5.000000e-01
  %d1 = fsub float 5.000000e-01, %y
  %a0 = insertvalue [2 x float] undef, float %d0, 0
  %a1 = insertvalue [2 x float] %a0, float %d1, 1
  %r0 = insertvalue <{ <4 x float>, [2 x float] }> undef, <4 x float> %c, 0
  %r1 = insertvalue <{ <4 x float>, [2 x float] }> %r0, [2 x float] %a1, 1
  ret <{ <4 x float>, [2 x float] }> %r1
}

!air.version = !{!1}
!air.language_version = !{!2}
!air.vertex = !{!3}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @cv, !4, !6}
!4 = !{!5, !9}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"p"}
!9 = !{!"air.clip_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"clip"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 96, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"corner"}
!8 = !{i32 1, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"v"}
