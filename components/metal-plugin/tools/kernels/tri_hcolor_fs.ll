; tri_hcolor_fs, a fragment function given a half4 varying (CoreAnimation's
; fixed_frag takes its colour so): returns it as it came. Paired with
; tri_hcolor_vs, which writes the colour tri_color_vs does as a half4, so the
; picture is tri_color's: a half varying linked or interpolated at the wrong size
; reads other bits as the colour.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_hcolor_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x half> @tri_hcolor_fs(<4 x half> %color) local_unnamed_addr {
  ret <4 x half> %color
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_hcolor_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4"}
!5 = !{!6}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_Dh)", !"air.center", !"air.perspective", !"air.arg_type_name", !"half4", !"air.arg_name", !"color"}
