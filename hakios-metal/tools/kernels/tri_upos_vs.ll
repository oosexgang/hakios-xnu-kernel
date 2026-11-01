; tri_upos_vs, vertex pulling from a constant buffer: the LLVM IR a Metal compiler
; would emit for
;
;     vertex float4 tri_upos_vs(constant float2 *pos [[buffer(0)]],
;                               uint vid             [[vertex_id]])
;     {
;       return float4(pos[vid], 0.0, 1.0);   // [[position]]
;     }
;
; The index is the vertex id, which no compile-time analysis bounds, so ir3 cannot
; push the buffer into the constant file (that is what tri_cflat_fs's fixed-offset
; read gets): it leaves an ldc through UBO descriptor 0, which the draw must load
; (ST6_UBO into the vertex stage's block, Gallium's fd6_emit_ubos). Paired with
; tri_flat_fs, and given tri_flat_vs's six positions in the buffer, the picture is
; 9b's pair exactly: positions read wrong would move the triangles.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention (tri_cflat_fs.ll for the buffer, tri_flat_vs.ll
; for the vertex id).
;
;   llvm-as tri_upos_vs.ll -o tri_upos_vs.bc
;   ir3_compiler --chip-id 0x06030500 tri_upos_vs.bc

source_filename = "tri_upos_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_upos_vs(ptr addrspace(2) %pos, i32 %vid) local_unnamed_addr {
  %i = zext i32 %vid to i64
  %p = getelementptr inbounds <2 x float>, ptr addrspace(2) %pos, i64 %i
  %xy = load <2 x float>, ptr addrspace(2) %p, align 8
  %x = extractelement <2 x float> %xy, i32 0
  %y = extractelement <2 x float> %xy, i32 1
  %v0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %x, i32 0
  %v1 = insertelement <4 x float> %v0, float %y, i32 1
  ret <4 x float> %v1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_upos_vs, !3, !5}
!3 = !{!4}
!4 = !{!"air.position", !"air.arg_type_name", !"float4"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"float2", !"air.arg_name", !"pos"}
!7 = !{i32 1, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
