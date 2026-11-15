; A post-tessellation vertex function over a triangle patch, drawn by tools/air/tesscheck.c with
; tools/air/tess_frag.ll: the point is w0 A + w1 B + w2 C for the corners A (-1, -1), B (1, -1) and
; C (-1, 1) of the target, and the colour is (w0, w1, w2, 1), so every pixel of the lower-left half
; must hold its own barycentric coordinates and the rest stay clear.
;
;     [[patch(triangle, 3)]] vertex Out tt(float3 w [[position_in_patch]]) {
;         return { float4(-w.x + w.y - w.z, -w.x - w.y + w.z, 0, 1), float4(w, 1) };
;     }
source_filename = "tesst"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define <{ <4 x float>, <4 x float> }> @tt(<3 x float> %0) local_unnamed_addr {
  %a = extractelement <3 x float> %0, i64 0
  %b = extractelement <3 x float> %0, i64 1
  %c = extractelement <3 x float> %0, i64 2
  %na = fneg float %a
  %x0 = fadd float %na, %b
  %x = fsub float %x0, %c
  %y0 = fsub float %na, %b
  %y = fadd float %y0, %c
  %p0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i64 0
  %p1 = insertelement <4 x float> %p0, float %y, i64 1
  %c0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %a, i64 0
  %c1 = insertelement <4 x float> %c0, float %b, i64 1
  %c2 = insertelement <4 x float> %c1, float %c, i64 2
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %p1, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %c2, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.vertex = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @tt, !4, !7, !10}
!4 = !{!5, !6}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{!8}
!8 = !{i32 0, !"air.position_in_patch", !"air.arg_type_name", !"float3", !"air.arg_name", !"w"}
!10 = !{!"air.patch", !"triangle", !"air.patch_control_point", i32 3}
