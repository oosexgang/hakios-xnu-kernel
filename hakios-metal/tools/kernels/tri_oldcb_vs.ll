; tri_oldcb_vs, a vertex function in the shape of AIR 2.3 (Metal 2.3, iOS 14): its buffer's metadata carries no
; air.address_space, which later AIR writes on every buffer. The LLVM IR a Metal compiler of that version would emit for
;
;     vertex float4 tri_oldcb_vs(uint vid [[vertex_id]], constant float4 *at [[buffer(1)]]) { return at[vid]; }
;
; The address space is then the pointer's own (addrspace(2), a constant buffer). tri_oldcb_vs and tri_olddb_vs differ
; in nothing else, so a front end that took one kind for every buffer without the key gets one of the two wrong.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_oldcb_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios14.0.0"

define <{ <4 x float> }> @tri_oldcb_vs(i32 %vid, ptr addrspace(2) %at) local_unnamed_addr {
  %i = zext i32 %vid to i64
  %p = getelementptr inbounds <4 x float>, ptr addrspace(2) %at, i64 %i
  %v = load <4 x float>, ptr addrspace(2) %p, align 16
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %r
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 3, i32 0}
!1 = !{!"Metal", i32 2, i32 3, i32 0}
!2 = !{ptr @tri_oldcb_vs, !3, !5}
!3 = !{!4}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"at"}
