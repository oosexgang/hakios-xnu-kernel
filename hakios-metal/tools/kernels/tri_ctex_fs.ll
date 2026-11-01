; tri_ctex_fs, a fragment function with a sampler of its own: the LLVM IR a Metal
; compiler would emit for
;
;     fragment float4 tri_ctex_fs(Vary in [[stage_in]], texture2d<float> t [[texture(0)]])
;     {
;       constexpr sampler s(address::repeat, filter::nearest);
;       return t.sample(s, in.color.xy * 4.0);   // [[color(0)]]
;     }
;
; A constexpr sampler is no argument: it is a module global, @__air_sampler_state,
; holding the sampler's state word (air_to_nir.c air_decode_sampler_state: the
; address modes s, t, r at bits 0, 3, 6, the filters, the lod range as two halves at
; 24 and 40), named in !air.sampler_states. This one is repeat on s and t, clamp to
; edge on r, nearest, mip none, lod [0, 65504]: 34901797600493650. The front end gives it the
; sampler index after the API's (0 here), and the runtime makes and binds it.
;
; The coordinate is the varying times 4, a computed value, so the sample is an
; ordinary sam rather than tri_tex_fs's prefetch -- and with tri_color_vs's (red,
; green) it runs over 0..4, where repeat and clamp to edge give different texels:
; repeat reads texel floor(16 u) mod 4, clamp would read texel 3 past u = 0.25.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; global and of the metadata is Apple's convention.
;
;   llvm-as tri_ctex_fs.ll -o tri_ctex_fs.bc
;   ir3_compiler --chip-id 0x06030500 tri_ctex_fs.bc

source_filename = "tri_ctex_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@__air_sampler_state = internal addrspace(2) constant i64 34901797600493650, align 8

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @tri_ctex_fs(<4 x float> %color, ptr addrspace(1) %t) local_unnamed_addr {
  %uv = shufflevector <4 x float> %color, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %uv4 = fmul <2 x float> %uv, <float 4.000000e+00, float 4.000000e+00>
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) @__air_sampler_state, <2 x float> %uv4, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}
!air.sampler_states = !{!10}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_ctex_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!10 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
