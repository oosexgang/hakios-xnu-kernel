; depth_fs: a fragment function that writes its own depth, the LLVM IR a Metal compiler emits for
;
;     struct Out { float4 color [[color(0)]]; float depth [[depth(any)]]; };
;     fragment Out depth_fs(float4 color [[stage_in]])
;     {
;       return Out{color, 0.25};
;     }
;
; Paired with tri_pos_vs: a triangle drawn at one depth whose fragments land at 0.25 whatever the rasterizer computed.
; Resident Evil 4's CopyDepthPS and its shadow passes write their depth so, which the draw refused. Hand-written, ours,
; MIT; only the shape of the metadata is Apple's convention (tri_color_fs.ll).

source_filename = "depth_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, float }> @depth_fs(<4 x float> %color) local_unnamed_addr {
  %r0 = insertvalue <{ <4 x float>, float }> undef, <4 x float> %color, 0
  %r1 = insertvalue <{ <4 x float>, float }> %r0, float 2.500000e-01, 1
  ret <{ <4 x float>, float }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @depth_fs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!"air.depth", !"air.depth_qualifier", !"any", !"air.arg_type_name", !"float"}
!6 = !{!7}
!7 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
