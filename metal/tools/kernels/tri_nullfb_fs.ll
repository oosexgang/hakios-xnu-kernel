; tri_nullfb_fs, a fragment function that both samples the front end's null texture and reads its render target: the
; shape of CoreAnimation's fixed_frag_lph_cpf, written as the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_nullfb_fs(float4 c [[color(0)]], texture2d<float> t [[texture(14)]], sampler s [[sampler(0)]])
;     {
;       float4 z = texture2d<float>().sample(s, float2(0));   // the null texture: zeros
;       return float4(c.g, c.b, c.r, 1) + z + t.sample(s, float2(0));
;     }
;
; A texture is sampled at slot 14, as CoreAnimation's reach that high, and the null texture's slot is the front end's
; next to it (15); the framebuffer read needs a slot of its own too. It used to be "one past the highest sampled
; slot" -- 16 here, past the table -- and is now the lowest free one.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_nullfb_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare ptr addrspace(1) @air.get_null_texture_2d()
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @tri_nullfb_fs(<4 x float> %c, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %t2 = tail call ptr addrspace(1) @air.get_null_texture_2d()
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t2, ptr addrspace(2) %s, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %z = extractvalue { <4 x float>, i8 } %r, 0
  %v = shufflevector <4 x float> %c, <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, <4 x i32> <i32 1, i32 2, i32 0, i32 7>
  %r15 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %s15 = extractvalue { <4 x float>, i8 } %r15, 0
  %o0 = fadd <4 x float> %v, %z
  %o = fadd <4 x float> %o0, %s15
  ret <4 x float> %o
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_nullfb_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
!8 = !{i32 1, !"air.texture", !"air.location_index", i32 14, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!7 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
