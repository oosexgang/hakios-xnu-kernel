; A dispatch whose last group is partial (Metal's -dispatchThreads:threadsPerThreadgroup:):
; 37 threads in groups of 16. Dispatched by tools/air/gridcheck.c. Each thread writes
; gid + 1000 * threads_per_grid + 100000 * dispatch_threads_per_threadgroup, so a thread that ran
; past the edge shows as a word that should have been left alone, a [[threads_per_grid]] of 48
; as 48000 where 37000 belongs, and the last group's own size as 5 rather than 16.
;
;     kernel void grid(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]],
;                      uint tpg [[threads_per_grid]],
;                      uint dtpt [[dispatch_threads_per_threadgroup]]) {
;         out[gid] = gid + 1000 * tpg + 100000 * dtpt;
;     }
source_filename = "grid"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @grid(ptr addrspace(1) %0, i32 %1, i32 %2, i32 %3) local_unnamed_addr {
  %k = mul i32 %2, 1000
  %d = mul i32 %3, 100000
  %s = add i32 %1, %k
  %v = add i32 %s, %d
  %w = zext i32 %1 to i64
  %p = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %w
  store i32 %v, ptr addrspace(1) %p, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @grid, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8, !9}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 256, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!8 = !{i32 2, !"air.threads_per_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tpg"}
!9 = !{i32 3, !"air.dispatch_threads_per_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"dtpt"}
