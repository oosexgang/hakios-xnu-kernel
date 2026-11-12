; The visible function linked_fs.ll calls: the colour it is given, with the position's x for alpha. Hand-written.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @custom_fn(<2 x float> %p, <4 x float> %c) local_unnamed_addr {
  %x = extractelement <2 x float> %p, i32 0
  %r = insertelement <4 x float> %c, float %x, i32 3
  ret <4 x float> %r
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.visible = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @custom_fn, !3, !5}
!3 = !{!4}
!4 = !{!"air.visible_output", !"air.arg_type_name", !"float4"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float2", !"air.arg_name", !"position"}
!7 = !{i32 1, !"air.visible_input", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
