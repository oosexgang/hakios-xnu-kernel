; The fragment half of tools/air/rast_tri.ll: red.
source_filename = "rastf"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define <{ <4 x float> }> @rf() local_unnamed_addr {
  ret <{ <4 x float> }> <{ <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0> }>
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @rf, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{}
