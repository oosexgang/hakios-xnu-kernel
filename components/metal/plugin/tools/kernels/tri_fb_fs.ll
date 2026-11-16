; tri_fb_fs, a fragment function that reads its own colour target (framebuffer
; fetch): the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_fb_fs(float4 c [[color(0)]])
;     {
;       return float4(c.g, c.b, c.r, 1);   // [[color(0)]]
;     }
;
; which is the shape of QuartzCore's luma_boost_lph (a layer re-coloured in place
; from what is under it), the pipeline CoreAnimation's window server exits on when
; it cannot be made. Paired with tri_flat_vs. Over a target cleared to 40 80 bf ff
; one draw leaves 80 bf 40 ff, and a second draw in the same pass bf 40 80 ff: the
; second must read what the first wrote, not the clear colour a stale cache would
; still hold, and a fetch that read nothing would give 00 00 00 ff.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata (an air.render_target among the fragment inputs) is Apple's convention.

source_filename = "tri_fb_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_fb_fs(<4 x float> %c) local_unnamed_addr {
  %v = shufflevector <4 x float> %c, <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, <4 x i32> <i32 1, i32 2, i32 0, i32 7>
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_fb_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
