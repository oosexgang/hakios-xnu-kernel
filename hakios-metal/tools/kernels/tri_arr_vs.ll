; tri_arr_vs, a vertex function that takes its corners from a constant array: the LLVM IR a Metal compiler would emit for
;
;     vertex float4 tri_arr_vs(uint vid [[vertex_id]])
;     {
;       constexpr constant float4 at[6] = { {-1,-1,0,1}, {1,-1,0,1}, {-1,1,0,1}, {1,-1,0,1}, {1,1,0,1}, {-1,1,0,1} };
;       return at[vid];
;     }
;
; tri_flat_vs's pair, with the positions in a module constant (address space 2) read at a run-time index, as sokol's
; bufferless triangle samples have them: drawn with tri_flat_fs, every pixel of the target is the flat colour.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_arr_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@at = internal addrspace(2) constant [6 x <4 x float>] [<4 x float> <float -1.0, float -1.0, float 0.0, float 1.0>, <4 x float> <float 1.0, float -1.0, float 0.0, float 1.0>, <4 x float> <float -1.0, float 1.0, float 0.0, float 1.0>, <4 x float> <float 1.0, float -1.0, float 0.0, float 1.0>, <4 x float> <float 1.0, float 1.0, float 0.0, float 1.0>, <4 x float> <float -1.0, float 1.0, float 0.0, float 1.0>], align 16

define <{ <4 x float> }> @tri_arr_vs(i32 %vid) local_unnamed_addr {
  %i = zext i32 %vid to i64
  %p = getelementptr inbounds [6 x <4 x float>], ptr addrspace(2) @at, i64 0, i64 %i
  %v = load <4 x float>, ptr addrspace(2) %p, align 16
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %r
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_arr_vs, !3, !5}
!3 = !{!4}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!6}
!6 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
