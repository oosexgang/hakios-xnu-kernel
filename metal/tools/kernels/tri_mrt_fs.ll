; tri_mrt_fs, a fragment function that writes two colour targets: the LLVM IR a
; Metal compiler would emit for
;
;     struct Out { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };
;     fragment Out tri_mrt_fs()
;     {
;       return Out{ float4(0x20, 0x40, 0x80, 0xff) / 255,   // tri_flat_fs's colour
;                   float4(1, 0, 0.25, 0.75) };
;     }
;
; which is the shape of QuartzCore's attachment_clear_frag_lph (two half4
; outputs, color0 and color1), the pipeline CoreAnimation's window server exits
; on when it cannot be made. Paired with tri_flat_vs. As RGBA8Unorm bytes the
; first target gets 20 40 80 ff and the second ff 00 40 bf -- neither a canary
; nor the probe's clear colour, and different from each other, so a target that
; got the other's colour, or none, shows.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata (one air.render_target per member of the returned struct) is Apple's
; convention.

source_filename = "tri_mrt_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define { <4 x float>, <4 x float> } @tri_mrt_fs() local_unnamed_addr {
  ret { <4 x float>, <4 x float> } { <4 x float> <float 0x3FC0101020000000, float 0x3FD0101020000000, float 0x3FE0101020000000, float 1.000000e+00>, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 2.500000e-01, float 7.500000e-01> }
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_mrt_fs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c0"}
!5 = !{!"air.render_target", i32 1, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c1"}
!6 = !{}
