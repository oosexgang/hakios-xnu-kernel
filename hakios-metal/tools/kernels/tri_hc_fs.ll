; tri_hc_fs, a fragment function whose half arithmetic reads a constant buffer's
; half straight from the const file: the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_hc_fs(float4 color [[stage_in]], constant half& s [[buffer(0)]])
;     {
;       return float4(float(half(color.r) * s) * 4, 0, 0, 1);
;     }
;
; which is the shape of CoreAnimation's noise dither (noise_scale, a half in a
; buffer of its own). ir3 pushes the half, its preamble widens it to f32 into a
; const register, and the main code reads it as a half operand (hc): right only
; when the SP demotes a const read at half precision (SP_MODE_CONTROL's
; CONSTANT_DEMOTION_ENABLE). With s = 0.25 the red is tri_color's red again; the
; f32 bits' low half read as a half would be 0 for 0.25.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_hc_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_hc_fs(<4 x float> %color, ptr addrspace(2) %s) local_unnamed_addr {
  %u = extractelement <4 x float> %color, i32 0
  %uh = fptrunc float %u to half
  %sh = load half, ptr addrspace(2) %s, align 2
  %m = fmul fast half %uh, %sh
  %mf = fpext half %m to float
  %r = fmul fast float %mf, 4.000000e+00
  %c = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %r, i32 0
  ret <4 x float> %c
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_hc_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 2, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"half", !"air.arg_name", !"s"}
