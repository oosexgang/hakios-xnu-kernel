; tri_pvt_fs, a fragment function whose array lives in private memory: the LLVM
; IR a Metal compiler would emit for
;
;     fragment float4 tri_pvt_fs(float4 pos [[position]])
;     {
;       uint x = uint(pos.x), y = uint(pos.y);
;       uint a[128];
;       for (uint i = 0; i < 128; i++) a[i] = 3 * i + y + 1;
;       return float4(a[x & 127] / 255.0, 0, 0, 1);
;     }
;
; which is the shape of CoreAnimation's path_exterior_fragment (512 bytes of
; private memory a fiber). 128 words are past the 256 bytes ir3 keeps in
; registers (nir_lower_vars_to_scratch in ir3_nir.c), the index is the pixel's,
; and the fill depends on y so that nir_opt_large_constants cannot make the array
; a constant table. Paired with tri_flat_vs. Pixel (i, j) must have red
; 3i + j + 1 (at most 253): private memory not bound or laid out wrong reads
; zeros or another fiber's words.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_pvt_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_pvt_fs(<4 x float> %pos) local_unnamed_addr {
entry:
  %a = alloca [128 x i32], align 4
  %px = extractelement <4 x float> %pos, i32 0
  %py = extractelement <4 x float> %pos, i32 1
  %x = fptoui float %px to i32
  %y = fptoui float %py to i32
  %base = add i32 %y, 1
  br label %fill

fill:
  %i = phi i32 [ 0, %entry ], [ %next, %fill ]
  %t = mul i32 %i, 3
  %v = add i32 %t, %base
  %p = getelementptr inbounds [128 x i32], ptr %a, i32 0, i32 %i
  store i32 %v, ptr %p, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 128
  br i1 %more, label %fill, label %read

read:
  %k = and i32 %x, 127
  %q = getelementptr inbounds [128 x i32], ptr %a, i32 0, i32 %k
  %w = load i32, ptr %q, align 4
  %f = uitofp i32 %w to float
  %r = fdiv float %f, 2.550000e+02
  %c = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %r, i32 0
  ret <4 x float> %c
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_pvt_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
