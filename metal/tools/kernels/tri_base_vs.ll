; tri_base_vs, a vertex function that reads the draw's own numbers: the LLVM IR a
; Metal compiler would emit for
;
;     struct out { float4 position [[position]]; float4 color; };
;     vertex out tri_base_vs(uint vid [[vertex_id]], uint bv [[base_vertex]],
;                            uint iid [[instance_id]])
;     {
;       out o;
;       o.position = tri_color_vs's corner (vid - bv);  // the pair, wherever the draw starts
;       o.color = float4(bv / 255.0, iid / 255.0, 0, 1);
;       return o;
;     }
;
; which is the shape of QuartzCore's path_blit_vert_lph: Metal's [[instance_id]]
; counts from the base instance, so the front end reads it as the hardware's
; zero-based id plus base_instance, and [[base_vertex]] as first_vertex -- both
; driver params, which the draw uploads every time. Paired with tri_color_fs.
; A draw of vertices 3..8, two instances from base instance 7, must give every
; pixel 03 08 00 ff: the second instance (7 + 1) drawn last. Params not uploaded
; read 0: the corners land elsewhere and the colour is 00 01 00 ff; the hardware's
; id not added, 03 07 00 ff.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata (air.base_vertex and air.instance_id among the vertex inputs) is
; Apple's convention.

source_filename = "tri_base_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @tri_base_vs(i32 %vid, i32 %bv, i32 %iid) local_unnamed_addr {
  %l = sub i32 %vid, %bv
  %c4 = icmp eq i32 %l, 4
  %c3 = icmp eq i32 %l, 3
  %c2 = icmp eq i32 %l, 2
  %c1 = icmp eq i32 %l, 1
  %c0 = icmp eq i32 %l, 0
  %p4 = select i1 %c4, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %p3 = select i1 %c3, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p4
  %p2 = select i1 %c2, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p3
  %p1 = select i1 %c1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %p0 = select i1 %c0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  %rf = uitofp i32 %bv to float
  %gf = uitofp i32 %iid to float
  %rn = fdiv float %rf, 2.550000e+02
  %gn = fdiv float %gf, 2.550000e+02
  %k0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %rn, i32 0
  %k1 = insertelement <4 x float> %k0, float %gn, i32 1
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %k1, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_base_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{!7, !8, !9}
!7 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!8 = !{i32 1, !"air.base_vertex", !"air.arg_type_name", !"uint", !"air.arg_name", !"bv"}
!9 = !{i32 2, !"air.instance_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"iid"}
