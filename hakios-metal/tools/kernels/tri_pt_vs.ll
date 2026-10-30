; tri_pt_vs, a vertex function for points: the LLVM IR a Metal compiler would emit for
;
;     struct P { float4 position [[position]]; float size [[point_size]]; };
;     vertex P tri_pt_vs(uint vid [[vertex_id]])
;     {
;       const float2 at[4] = { float2(-0.5, 0.5), float2(0.5, 0.5), float2(-0.5, -0.5), float2(0.5, -0.5) };
;       return P{ float4(at[vid], 0, 1), 8.0 };
;     }
;
; Four points at the centres of a 64x64 target's quarters -- pixels (16, 16), (48, 16), (16, 48), (48, 48), Metal's y
; up -- each 8 pixels across: drawn as points, 4 squares of 8x8 and nothing else.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_pt_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, float }> @tri_pt_vs(i32 %vid) local_unnamed_addr {
  %c2 = icmp eq i32 %vid, 2
  %c1 = icmp eq i32 %vid, 1
  %c0 = icmp eq i32 %vid, 0
  %p2 = select i1 %c2, <4 x float> <float -5.000000e-01, float -5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float 5.000000e-01, float -5.000000e-01, float 0.000000e+00, float 1.000000e+00>
  %p1 = select i1 %c1, <4 x float> <float 5.000000e-01, float 5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %p0 = select i1 %c0, <4 x float> <float -5.000000e-01, float 5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  %r0 = insertvalue <{ <4 x float>, float }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, float }> %r0, float 8.000000e+00, 1
  ret <{ <4 x float>, float }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_pt_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.point_size", !"air.arg_type_name", !"float", !"air.arg_name", !"size"}
!6 = !{!7}
!7 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
