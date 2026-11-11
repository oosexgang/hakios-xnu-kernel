; tri_fw_fs, a fragment function that takes the screen-space derivative of a
; varying, in float and in half: the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_fw_fs(float4 color [[stage_in]])
;     {
;       float f = fwidth(color.r);
;       half h = fwidth(half(color.r));
;       return float4(f * 32, float(h) * 32, 0, 1);
;     }
;
; which is the shape of CoreAnimation's rounded-rect coverage (fwidth of a half
; texcoord in fixed_frag). Paired with tri_color_vs, whose red is (y - x) / 64 in
; T1 and y / 64 in T2: fwidth is 1/64 + 1/64 = 1/32 in T1 and 1/64 in T2, so
; every pixel away from the diagonal is ff ff 00 ff in T1 and 80 80 00 ff in T2.
; A derivative read across lanes that do not hold the quad's other pixels gives
; anything else, and one of 0 gives 00 00 00 ff.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_fw_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare float @air.fwidth.f32(float)
declare half @air.fwidth.f16(half)

define <4 x float> @tri_fw_fs(<4 x float> %color) local_unnamed_addr {
  %u = extractelement <4 x float> %color, i32 0
  %f = tail call fast float @air.fwidth.f32(float %u)
  %uh = fptrunc float %u to half
  %h = tail call fast half @air.fwidth.f16(half %uh)
  %hf = fpext half %h to float
  %a = fmul fast float %f, 3.200000e+01
  %b = fmul fast float %hf, 3.200000e+01
  %c0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %a, i32 0
  %c1 = insertelement <4 x float> %c0, float %b, i32 1
  ret <4 x float> %c1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_fw_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
