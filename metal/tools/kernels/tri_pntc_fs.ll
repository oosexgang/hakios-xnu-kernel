; tri_pntc_fs, a fragment function that reads its point sprite coordinate: the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_pntc_fs(float2 pc [[point_coord]])
;     {
;       return float4(pc, 0, 1);
;     }
;
; [[point_coord]] reaches ir3 only as a varying the VPC fills (VARYING_SLOT_PNTC, compiler.cpp); as the system value
; the front end emits, ir3 has no case for it and stops the process (CoreAnimation's fixed_frag_lph_cpf did so to
; backboardd). The link gives the input PS_REPL S and T in VPC_VARYING_PS_REPL_MODE.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_pntc_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_pntc_fs(<2 x float> %pc) local_unnamed_addr {
  %v = shufflevector <2 x float> %pc, <2 x float> <float 0.0, float 1.0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_pntc_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.point_coord", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"pc"}
