; dual_fs: a fragment function with a second source colour for dual-source blending, the LLVM IR a Metal compiler
; emits for
;
;     struct Out { float4 a [[color(0), index(0)]]; float4 b [[color(0), index(1)]]; };
;     fragment Out dual_fs(float4 color [[stage_in]])
;     {
;       return Out{color, float4(0.5, 0.25, 1.0, 0.75)};
;     }
;
; With the blend Source1Color * source + 0, attachment 0 gets each channel of the colour times (0.5, 0.25, 1, 0.75):
; numbers no single-source blend of these two gives. Resident Evil 4 blends so, which the pipeline refused.
; Hand-written, ours, MIT; only the shape of the metadata is Apple's convention (tri_color_fs.ll).

source_filename = "dual_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @dual_fs(<4 x float> %color) local_unnamed_addr {
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %color, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> <float 5.000000e-01, float 2.500000e-01, float 1.000000e+00, float 7.500000e-01>, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @dual_fs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!"air.render_target", i32 0, i32 1, !"air.arg_type_name", !"float4"}
!6 = !{!7}
!7 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
