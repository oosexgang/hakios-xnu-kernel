; A post-tessellation vertex function whose quad patch's four control points come through [[stage_in]]
; (`patch_control_point<CP>`), read by the function Metal makes out of the vertex descriptor -- the
; shape of LocalAuthenticationPrivateUI's spline renderer. Each point is lerped between the patch's
; corners, so with patch 0's corners (-1,-1) (0,-1) (-1,1) (0,1) and patch 1's one unit to the right
; the picture is tools/air/tess_quad.ll's: drawn by tools/air/tesscheck.c with `cp`.
;
;     struct CP { float2 pos [[attribute(0)]]; };
;     [[patch(quad, 4)]] vertex Out tc(patch_control_point<CP> cps [[stage_in]],
;                                      float2 uv [[position_in_patch]], uint p [[patch_id]]) {
;         float2 a = mix(cps[0].pos, cps[1].pos, uv.x), b = mix(cps[2].pos, cps[3].pos, uv.x);
;         return { float4(mix(a, b, uv.y), 0, 1), float4(uv, p, 1) };
;     }
source_filename = "tess_cp"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare { <2 x float> } @cp.MTL_CONTROL_POINT_FN(i32, ptr) section "air.externally_defined"

define <{ <4 x float>, <4 x float> }> @tc(ptr %cps, <2 x float> %uv, i32 %p) local_unnamed_addr {
  %r0 = tail call { <2 x float> } @cp.MTL_CONTROL_POINT_FN(i32 0, ptr %cps)
  %r1 = tail call { <2 x float> } @cp.MTL_CONTROL_POINT_FN(i32 1, ptr %cps)
  %r2 = tail call { <2 x float> } @cp.MTL_CONTROL_POINT_FN(i32 2, ptr %cps)
  %r3 = tail call { <2 x float> } @cp.MTL_CONTROL_POINT_FN(i32 3, ptr %cps)
  %c0 = extractvalue { <2 x float> } %r0, 0
  %c1 = extractvalue { <2 x float> } %r1, 0
  %c2 = extractvalue { <2 x float> } %r2, 0
  %c3 = extractvalue { <2 x float> } %r3, 0
  %u = extractelement <2 x float> %uv, i64 0
  %v = extractelement <2 x float> %uv, i64 1
  %us = insertelement <2 x float> undef, float %u, i64 0
  %uu = shufflevector <2 x float> %us, <2 x float> undef, <2 x i32> zeroinitializer
  %vs = insertelement <2 x float> undef, float %v, i64 0
  %vv = shufflevector <2 x float> %vs, <2 x float> undef, <2 x i32> zeroinitializer
  %d01 = fsub <2 x float> %c1, %c0
  %m01 = fmul <2 x float> %d01, %uu
  %a = fadd <2 x float> %c0, %m01
  %d23 = fsub <2 x float> %c3, %c2
  %m23 = fmul <2 x float> %d23, %uu
  %b = fadd <2 x float> %c2, %m23
  %dab = fsub <2 x float> %b, %a
  %mab = fmul <2 x float> %dab, %vv
  %xy = fadd <2 x float> %a, %mab
  %x = extractelement <2 x float> %xy, i64 0
  %y = extractelement <2 x float> %xy, i64 1
  %pf = uitofp i32 %p to float
  %p0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i64 0
  %p1 = insertelement <4 x float> %p0, float %y, i64 1
  %k0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %u, i64 0
  %k1 = insertelement <4 x float> %k0, float %v, i64 1
  %k2 = insertelement <4 x float> %k1, float %pf, i64 2
  %o0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %p1, 0
  %o1 = insertvalue <{ <4 x float>, <4 x float> }> %o0, <4 x float> %k2, 1
  ret <{ <4 x float>, <4 x float> }> %o1
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.vertex = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @tc, !4, !7, !13}
!4 = !{!5, !6}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{!8, !11, !12}
!8 = !{i32 0, !"air.patch_control_point_input", !9, !10}
!9 = !{!"air.patch_control_point_function", ptr @cp.MTL_CONTROL_POINT_FN}
!10 = !{!"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"pos"}
!11 = !{i32 1, !"air.position_in_patch", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!12 = !{i32 2, !"air.patch_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"p"}
!13 = !{!"air.patch", !"quad", !"air.patch_control_point", i32 4}
