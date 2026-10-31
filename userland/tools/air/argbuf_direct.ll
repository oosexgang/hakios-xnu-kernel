; The control for tools/air/argbuf_set.ll: THE SAME SHADER with direct arguments.
;
; Every channel is computed from the same resource with the same instruction at the same
; coordinate; the only difference is how the resource reaches the shader -- five
; `[[buffer(0)]]`, `[[sampler(0)]]`, `[[texture(0)]]`, `[[texture(1)]]`,
; `[[sampler(1)]]` arguments here against five members of one argument buffer there.
;
;     fragment float4 argbuf_direct(constant Params *params  [[buffer(0)]],
;                                   sampler nearest          [[sampler(0)]],
;                                   texture2d<float> texA    [[texture(0)]],
;                                   texture2d<float> texB    [[texture(1)]],
;                                   sampler linear           [[sampler(1)]])
;     { return float4(texA.sample(nearest, float2(0.3125, 0.375)).x,
;                     texB.sample(linear,  float2(0.3125, 0.375)).x,
;                     params->v,
;                     texA.read(uint2(2, 3)).x); }
;
; The indices are the ones the FRONTEND allocates for that set's members, which is what
; makes the two comparable: one flat counter per kind in (set, member) order, so the
; buffer member is buffer 0, the samplers are 0 and 1 in member order and the textures
; are 0 and 1 (docs/air.md 8.2.1). This module is bound at those indices directly.
;
; So the measurement is differential and needs no oracle: the two draws must produce the
; SAME FOUR FLOATS. If they do, the encoder's records delivered the same five resources
; to the same five slots; if they do not, the picture says which channel moved and
; therefore which member. air_metal_test.c --draw-argbuf runs both and also runs the
; deliberately wrong records, whose pictures are named there.
;
;   llvm-as -o argbuf_direct.bc tools/air/argbuf_direct.ll

source_filename = "argbuf_direct"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios14.0.0"

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32) local_unnamed_addr

define <{ <4 x float> }> @argbuf_direct(ptr addrspace(2) %0, ptr addrspace(2) %1, ptr addrspace(1) %2, ptr addrspace(1) %3, ptr addrspace(2) %4) local_unnamed_addr {
  %cv = load float, ptr addrspace(2) %0, align 4

  %r1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %2, ptr addrspace(2) %1, <2 x float> <float 3.125000e-01, float 3.750000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %r1, 0
  %c1 = extractelement <4 x float> %v1, i64 0

  %r2 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %3, ptr addrspace(2) %4, <2 x float> <float 3.125000e-01, float 3.750000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %r2, 0
  %c2 = extractelement <4 x float> %v2, i64 0

  %r3 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %2, <2 x i32> <i32 2, i32 3>, i32 0, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %r3, 0
  %c3 = extractelement <4 x float> %v3, i64 0

  %o0 = insertelement <4 x float> undef, float %c1, i64 0
  %o1 = insertelement <4 x float> %o0, float %c2, i64 1
  %o2 = insertelement <4 x float> %o1, float %cv, i64 2
  %o3 = insertelement <4 x float> %o2, float %c3, i64 3
  %out = insertvalue <{ <4 x float> }> undef, <4 x float> %o3, 0
  ret <{ <4 x float> }> %out
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 3, i32 0}
!1 = !{!"Metal", i32 2, i32 3, i32 0}
!2 = !{ptr @argbuf_direct, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6, !7, !8, !9, !10}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"type_Params", !"air.arg_name", !"params"}
!7 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"nearestSampler"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"texA"}
!9 = !{i32 3, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"texB"}
!10 = !{i32 4, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"linearSampler"}
