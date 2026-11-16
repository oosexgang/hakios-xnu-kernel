; tri_vtex_vs, a vertex function that samples a texture: the LLVM IR a Metal compiler would emit for
;
;     struct P { float4 position [[position]]; float size [[point_size]]; float4 color; };
;     vertex P tri_vtex_vs(uint vid [[vertex_id]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
;     {
;       const float2 at[4] = { float2(-0.5, 0.5), float2(0.5, 0.5), float2(-0.5, -0.5), float2(0.5, -0.5) };
;       return P{ float4(at[vid], 0, 1), 8.0, t.sample(s, float2((vid + 0.5) / 4, 0.5), level(0)) };
;     }
;
; tri_pt_vs's four points, each the colour of texel VID of a 4x1 texture under a nearest sampler: with tri_color_fs
; each 8x8 square is its own texel, so a vertex stage that read nothing, or one texel for all four, or the texels in
; another order, draws something else. A vertex stage samples at an explicit level, Metal's rule, hence level(0).
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_vtex_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <{ <4 x float>, float, <4 x float> }> @tri_vtex_vs(i32 %vid, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %c2 = icmp eq i32 %vid, 2
  %c1 = icmp eq i32 %vid, 1
  %c0 = icmp eq i32 %vid, 0
  %p2 = select i1 %c2, <4 x float> <float -5.000000e-01, float -5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float 5.000000e-01, float -5.000000e-01, float 0.000000e+00, float 1.000000e+00>
  %p1 = select i1 %c1, <4 x float> <float 5.000000e-01, float 5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %p0 = select i1 %c0, <4 x float> <float -5.000000e-01, float 5.000000e-01, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  %vf = uitofp i32 %vid to float
  %vh = fadd float %vf, 5.000000e-01
  %u = fmul float %vh, 2.500000e-01
  %uv0 = insertelement <2 x float> <float poison, float 5.000000e-01>, float %u, i32 0
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <2 x float> %uv0, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %k = extractvalue { <4 x float>, i8 } %r, 0
  %r0 = insertvalue <{ <4 x float>, float, <4 x float> }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, float, <4 x float> }> %r0, float 8.000000e+00, 1
  %r2 = insertvalue <{ <4 x float>, float, <4 x float> }> %r1, <4 x float> %k, 2
  ret <{ <4 x float>, float, <4 x float> }> %r2
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_vtex_vs, !3, !7}
!3 = !{!4, !5, !6}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.point_size", !"air.arg_type_name", !"float", !"air.arg_name", !"size"}
!6 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{!8, !9, !10}
!8 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!9 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!10 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
