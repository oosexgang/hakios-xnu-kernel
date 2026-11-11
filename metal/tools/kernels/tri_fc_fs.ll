; tri_fc_fs, a fragment function specialised by the pipeline's function constants:
; the LLVM IR a Metal compiler would emit for
;
;     constant int  fc_value [[function_constant(0)]];   // guarded: may be absent
;     constant bool fc_opaque [[function_constant(1)]];  // read as it is: the pipeline must supply it
;     fragment float4 tri_fc_fs()
;     {
;       return float4(float(fc_value) / 255, is_function_constant_defined(fc_value) ? 1 : 0, 0.25,
;                     fc_opaque ? 1 : 0.25);   // [[color(0)]]
;     }
;
; Paired with tri_flat_vs. As RGBA8Unorm bytes, each set of constants gives its
; own pixel, so the pipeline's values and the "was it supplied" answer are both
; read off the target:
;
;     nothing supplied        00 00 40 40   (the guarded one absent, the bool zero)
;     0 = 7, 1 = true         07 ff 40 ff
;     0 = 0                   00 ff 40 40   (supplied, and zero: not the same as absent)
;     1 = true                00 00 40 ff
;
; The declarations end in `i1 false` for the guarded constant and `i1 true` for
; the one read without asking, which is how Metal marks a constant the pipeline
; may leave out against one it must give.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata (!air.function_constants, the air.fc_initializer globals) and of the
; intrinsic's signature is Apple's convention.

source_filename = "tri_fc_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@fc_value.MTL_FC_INIT_0_i = internal addrspace(2) externally_initialized constant i32 undef, section "air.fc_initializer", align 4
@fc_opaque.MTL_FC_INIT_1_b = internal addrspace(2) externally_initialized constant i8 undef, section "air.fc_initializer", align 1

declare i1 @air.is_function_constant_defined(ptr addrspace(2) nocapture) local_unnamed_addr

define <4 x float> @tri_fc_fs() local_unnamed_addr {
  %v = load i32, ptr addrspace(2) @fc_value.MTL_FC_INIT_0_i, align 4
  %d = tail call i1 @air.is_function_constant_defined(ptr addrspace(2) @fc_value.MTL_FC_INIT_0_i)
  %ob = load i8, ptr addrspace(2) @fc_opaque.MTL_FC_INIT_1_b, align 1
  %o = trunc i8 %ob to i1
  %vf = uitofp i32 %v to float
  %r = fdiv float %vf, 2.550000e+02
  %g = select i1 %d, float 1.000000e+00, float 0.000000e+00
  %a = select i1 %o, float 1.000000e+00, float 2.500000e-01
  %c0 = insertelement <4 x float> undef, float %r, i32 0
  %c1 = insertelement <4 x float> %c0, float %g, i32 1
  %c2 = insertelement <4 x float> %c1, float 2.500000e-01, i32 2
  %c3 = insertelement <4 x float> %c2, float %a, i32 3
  ret <4 x float> %c3
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}
!air.function_constants = !{!6, !7}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_fc_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{}
!6 = !{ptr addrspace(2) @fc_value.MTL_FC_INIT_0_i, !"int", !"fc_value", i32 0, i1 false}
!7 = !{ptr addrspace(2) @fc_opaque.MTL_FC_INIT_1_b, !"bool", !"fc_opaque", i32 1, i1 true}
