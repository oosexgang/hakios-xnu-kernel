; tri_tex3d_fs, a fragment function that samples a 3D texture: the LLVM IR a Metal compiler would emit for
;
;     struct Vary { float4 position [[position]]; float4 color; };
;     fragment float4 tri_tex3d_fs(Vary in [[stage_in]], texture3d<float> t [[texture(0)]], sampler s [[sampler(0)]])
;     {
;       return t.sample(s, float3(in.color.xy, 0.625));
;     }
;
; Paired with tri_color_vs as tri_tex_fs is: red and green are the texel's x and y; 0.625 is the centre of slice 2 of
; a 4-deep texture, so under nearest every pixel reads a texel of that slice -- one of another slice says the slice
; pitch or the depth is wrong (CoreAnimation's colour lookup tables are 3D textures, read this way).
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_tex3d_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i1, <3 x i32>, i1, float, float, i32)

define <4 x float> @tri_tex3d_fs(<4 x float> %color, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %uvw = shufflevector <4 x float> %color, <4 x float> <float 0.0, float 0.0, float 0.625, float 0.0>, <3 x i32> <i32 0, i32 1, i32 6>
  %r = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <3 x float> %uvw, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_tex3d_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture3d<float, sample>", !"air.arg_name", !"t"}
!8 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
