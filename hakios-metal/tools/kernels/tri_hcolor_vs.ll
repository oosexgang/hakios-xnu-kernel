; tri_hcolor_vs, tri_color_vs with its colour a half4: the LLVM IR a Metal compiler
; would emit for tri_color_vs with `half4 color` in its output struct, which is the
; shape of CoreAnimation's fixed_vert (its colour and texcoords are half). Paired
; with tri_hcolor_fs, which returns the half4 it is given: tri_color's picture.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_hcolor_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x half> }> @tri_hcolor_vs(i32 %vid) local_unnamed_addr {
  %c4 = icmp eq i32 %vid, 4
  %c3 = icmp eq i32 %vid, 3
  %c2 = icmp eq i32 %vid, 2
  %c1 = icmp eq i32 %vid, 1
  %c0 = icmp eq i32 %vid, 0
  %p4 = select i1 %c4, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %p3 = select i1 %c3, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p4
  %p2 = select i1 %c2, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p3
  %p1 = select i1 %c1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %p0 = select i1 %c0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  %k4 = select i1 %c4, <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>
  %k3 = select i1 %c3, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k4
  %k2 = select i1 %c2, <4 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>, <4 x float> %k3
  %k1 = select i1 %c1, <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k2
  %k0 = select i1 %c0, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k1
  %kh = fptrunc <4 x float> %k0 to <4 x half>
  %r0 = insertvalue <{ <4 x float>, <4 x half> }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, <4 x half> }> %r0, <4 x half> %kh, 1
  ret <{ <4 x float>, <4 x half> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_hcolor_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(5colorDv4_Dh)", !"air.arg_type_name", !"half4", !"air.arg_name", !"color"}
!6 = !{!7}
!7 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
