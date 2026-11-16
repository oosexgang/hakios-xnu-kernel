; tri_tex39_fs, a fragment function that samples two textures at slots other than
; 0 with two samplers: the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_tex39_fs(texture2d<float> a [[texture(3)]],
;                                  texture2d<float> b [[texture(9)]],
;                                  sampler sa [[sampler(0)]], sampler sb [[sampler(1)]])
;     {
;       return float4(a.sample(sa, float2(0.5)).r, b.sample(sb, float2(0.5)).g, 0, 1);
;     }
;
; which is the shape of CoreAnimation's fixed_frag with a noise texture: its image at
; [[texture(3)]] and its noise at [[texture(9)]], each with a sampler of its own. With
; a 1x1 texture of ff 00 00 ff at 3 and one of 00 80 00 ff at 9 every pixel is
; ff 80 00 ff; the two read the other way round give 00 00 00 ff, and the same one
; twice ff 00 00 ff or 00 80 00 ff.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_tex39_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @tri_tex39_fs(ptr addrspace(1) %a, ptr addrspace(1) %b, ptr addrspace(2) %sa, ptr addrspace(2) %sb) local_unnamed_addr {
  %ra = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %a, ptr addrspace(2) %sa, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %rb = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %b, ptr addrspace(2) %sb, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %va = extractvalue { <4 x float>, i8 } %ra, 0
  %vb = extractvalue { <4 x float>, i8 } %rb, 0
  %x = extractelement <4 x float> %va, i32 0
  %y = extractelement <4 x float> %vb, i32 1
  %c0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %x, i32 0
  %c1 = insertelement <4 x float> %c0, float %y, i32 1
  ret <4 x float> %c1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_tex39_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8, !9}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"a"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 9, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"b"}
!8 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"sa"}
!9 = !{i32 3, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"sb"}
