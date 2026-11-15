; A post-tessellation vertex function over a quad patch, and the fragment function it feeds, drawn by
; tools/air/tesscheck.c: patch p puts its point at x = -1 + u + p, y = -1 + 2v, so patch 0 covers the
; left half of the target and patch 1 the right, and passes (u, v, p, 1) on as its colour.
;
;     struct Out { float4 position [[position]]; float4 color; };
;     [[patch(quad, 4)]] vertex Out tv(float2 uv [[position_in_patch]], uint p [[patch_id]]) {
;         return { float4(-1 + uv.x + p, -1 + 2 * uv.y, 0, 1), float4(uv, p, 1) };
;     }
;     fragment float4 tf(Out in [[stage_in]]) { return in.color; }
source_filename = "tess"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define <{ <4 x float>, <4 x float> }> @tv(<2 x float> %0, i32 %1) local_unnamed_addr {
  %u = extractelement <2 x float> %0, i64 0
  %v = extractelement <2 x float> %0, i64 1
  %pf = uitofp i32 %1 to float
  %x0 = fadd float %u, -1.000000e+00
  %x = fadd float %x0, %pf
  %y0 = fmul float %v, 2.000000e+00
  %y = fadd float %y0, -1.000000e+00
  %p0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i64 0
  %p1 = insertelement <4 x float> %p0, float %y, i64 1
  %c0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %u, i64 0
  %c1 = insertelement <4 x float> %c0, float %v, i64 1
  %c2 = insertelement <4 x float> %c1, float %pf, i64 2
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
!3 = !{ptr @tv, !4, !7, !10}
!4 = !{!5, !6}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{!8, !9}
!8 = !{i32 0, !"air.position_in_patch", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!9 = !{i32 1, !"air.patch_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"p"}
!10 = !{!"air.patch", !"quad", !"air.patch_control_point", i32 4}
