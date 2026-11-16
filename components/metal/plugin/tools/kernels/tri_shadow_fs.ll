; tri_shadow_fs, a fragment function that compares a depth texture with a value: the LLVM IR a Metal compiler would
; emit for
;
;     fragment float4 tri_shadow_fs(float4 pos [[position]], constant float &ref [[buffer(0)]],
;                                   depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
;     {
;       return float4(t.sample_compare(s, pos.xy / 64.0, ref), 0, 0, 1);
;     }
;
; Each pixel of a 64x64 target compares its own texel of a 64x64 depth texture (a shadow map's lookup, nearest) with
; REF under the sampler's compare function: red 1 where the comparison passes, 0 where it fails. A depth texture
; holding two depths either side of REF splits the target along where they meet; one that was never read, or read as
; a colour texture, does not.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention (the
; depth form's i32 after the sampler as Apple's declaration of air.sample_depth_2d has it).

source_filename = "tri_shadow_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @tri_shadow_fs(<4 x float> %pos, ptr addrspace(2) %ref, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %xy = shufflevector <4 x float> %pos, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %uv = fmul <2 x float> %xy, <float 1.562500e-02, float 1.562500e-02>
  %r = load float, ptr addrspace(2) %ref, align 4
  %c = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %t, ptr addrspace(2) %s, i32 1, <2 x float> %uv, float %r, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { float, i8 } %c, 0
  %o0 = insertelement <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %v, i32 0
  ret <4 x float> %o0
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_shadow_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8, !9}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"ref"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"t"}
!9 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
