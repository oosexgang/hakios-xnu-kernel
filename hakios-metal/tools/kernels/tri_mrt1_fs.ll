; tri_mrt1_fs, a fragment function that writes [[color(1)]] and nothing else: the
; LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_mrt1_fs() [[color(1)]] { return float4(1, 0, 0.25, 0.75); }
;
; which is the shape CoreAnimation's fixed_frag_lph_cph takes with fc_dest_index 1:
; it writes the second target and reads the first through framebuffer fetch. The
; first target has no output to write, so the RB must write nothing there: an
; attachment 0 seeded with a canary keeps it, and attachment 1 gets ff 00 40 bf.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata (one air.render_target, at index 1) is Apple's convention.

source_filename = "tri_mrt1_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_mrt1_fs() local_unnamed_addr {
  ret <4 x float> <float 1.000000e+00, float 0.000000e+00, float 2.500000e-01, float 7.500000e-01>
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_mrt1_fs, !3, !6}
!3 = !{!5}
!5 = !{!"air.render_target", i32 1, i32 0, !"air.arg_type_name", !"float4"}
!6 = !{}
