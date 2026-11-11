; width_id: the thread execution width a kernel is given, the LLVM IR a Metal compiler emits for
;
;     kernel void width_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]],
;                          uint width [[thread_execution_width]])
;     {
;       out[gid] = width * 1000 + (gid & 63);
;     }
;
; The width is the SIMD-group's: 64 on the A635 in every stage (hakios-metal keeps ir3 to its single threadsize), the
; number the device reports as the pipeline's threadExecutionWidth. Resident Evil 4's mainCS and
; CS_MiniClusterCompaction take it, which the front end refused as an input it did not know. The gid term makes every
; word differ, so a kernel that wrote one word for all would show. Hand-written, ours, MIT; the metadata's shape is
; Apple's convention, as in fill_id.ll.

source_filename = "width_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @width_id(ptr addrspace(1) %out, i32 %gid, i32 %width) local_unnamed_addr {
  %w = mul i32 %width, 1000
  %l = and i32 %gid, 63
  %v = add i32 %w, %l
  %t = zext i32 %gid to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %v, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @width_id, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!7 = !{i32 2, !"air.thread_execution_width", !"air.arg_type_name", !"uint", !"air.arg_name", !"width"}
