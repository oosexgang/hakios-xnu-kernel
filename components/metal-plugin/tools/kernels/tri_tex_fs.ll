; tri_tex_fs, the first fragment function that samples: the LLVM IR a Metal
; compiler would emit for
;
;     struct Vary { float4 position [[position]]; float4 color; };
;     fragment float4 tri_tex_fs(Vary in [[stage_in]],
;                                texture2d<float> t [[texture(0)]],
;                                sampler s          [[sampler(0)]])
;     {
;       return t.sample(s, in.color.xy);   // [[color(0)]]
;     }
;
; Paired with tri_color_vs, whose colour varying is red at v0/v3, green at v1/v4
; and blue at v2/v5: its red and green components, interpolated, ARE a texture
; coordinate -- (1, 0) at one corner, (0, 1) at the next, (0, 0) at the third --
; so every pixel samples a texel the probe can name from the barycentric rule it
; already has (tri_color's), with no new vertex function. Under nearest/clamp the
; expectation is that texel exactly.
;
; Why the coordinate is the varying itself and nothing computed from it: that is
; the shape ir3 turns into a prefetch (ir3_nir_lower_tex_prefetch: a 2D sample
; whose coordinate is an interpolated input, nothing else), the sample the FS
; issues before its first instruction through SP_FS_PREFETCH_CMD. The prefetch is
; what the fragment path needs first: most of what Metal's clients draw is a
; textured quad sampled at a varying, the shape every compositor's blit has.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata and of the intrinsic signature is Apple's convention (tri_color_fs.ll
; for the varying, tex_sample_gid.ll for the sample).
;
;   llvm-as tri_tex_fs.ll -o tri_tex_fs.bc
;   ir3_compiler --chip-id 0x06030500 tri_tex_fs.bc

source_filename = "tri_tex_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @tri_tex_fs(<4 x float> %color, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %uv = shufflevector <4 x float> %color, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_tex_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!8 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
