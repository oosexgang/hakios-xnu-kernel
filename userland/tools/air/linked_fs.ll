; A fragment function that calls a visible function it only declares, linked at pipeline time (MTLLinkedFunctions):
; the shape of RenderBox's custom effects (custom_effect_fragment: `custom_fn.MTL_VISIBLE_FN_REF`, named "custom_fn"
; by !air.visible_function_references). Hand-written, not Apple's; linked_fn.ll is the function it links.
;
;   llvm-as linked_fs.ll -o /tmp/linked_fs.bc; llvm-as linked_fn.ll -o /tmp/linked_fn.bc
;   air_run --link custom_fn=/tmp/linked_fn.bc /tmp/linked_fs.bc
;
; The pixel at (0, 0) must be 0.5 0.25 0.125 0.5: the colour this function passes, with the alpha the linked function
; puts there, the x of the position it is given. Without --link the front end refuses the call by name. (Floats and
; not RenderBox's halves: softpipe, which runs it here, has no f2f16.)

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @linkedFs(<4 x float> %pos) local_unnamed_addr {
  %xy = shufflevector <4 x float> %pos, <4 x float> undef, <2 x i32> <i32 0, i32 1>
  %c = tail call <4 x float> @custom_fn.MTL_VISIBLE_FN_REF(<2 x float> %xy, <4 x float> <float 0.5, float 0.25, float 0.125, float 1.0>)
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %c, 0
  ret <{ <4 x float> }> %r
}

declare <4 x float> @custom_fn.MTL_VISIBLE_FN_REF(<2 x float>, <4 x float>) local_unnamed_addr section "air.externally_defined"

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}
!air.visible_function_references = !{!7}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @linkedFs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!7 = !{!"air.visible_function_reference", ptr @custom_fn.MTL_VISIBLE_FN_REF, !"custom_fn"}
