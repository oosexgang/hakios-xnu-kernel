; Pointers in a local array at indices the shader computes, as MPS's LSTM kernels keep their inputs: a
; table of two buffers filled at constant indices, copied into a second table in a loop, and read back
; at t & 1 -- checked by tools/air/wordcheck.sh (buffer 1 holds 1000 + i): thread t writes the word t
; of whichever its index chose, so out[t] is t's own (zero, buffer 0 starts zeroed) for even t and
; 1000 + t for odd t.
;
;     kernel void pt(device uint *out [[buffer(0)]], constant uint *in [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) {
;         const device uint *tab[2] = { out, in }, *copy[2];
;         for (uint i = 0; i < 2; i++) copy[i] = tab[i];
;         uint v = copy[t & 1][t];
;         out[8 + t] = v; }
source_filename = "pt"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @pt(ptr addrspace(1) %0, ptr addrspace(2) %1, i32 %t) local_unnamed_addr {
  %tab = alloca [2 x ptr addrspace(1)], align 8
  %copy = alloca [2 x ptr addrspace(1)], align 8
  %t0 = getelementptr inbounds [2 x ptr addrspace(1)], ptr %tab, i64 0, i64 0
  store ptr addrspace(1) %0, ptr %t0, align 8
  %t1 = getelementptr inbounds [2 x ptr addrspace(1)], ptr %tab, i64 0, i64 1
  %in = addrspacecast ptr addrspace(2) %1 to ptr addrspace(1)
  store ptr addrspace(1) %in, ptr %t1, align 8
  br label %loop
loop:
  %i = phi i64 [ 0, %2 ], [ %next, %loop ]
  %src = getelementptr inbounds [2 x ptr addrspace(1)], ptr %tab, i64 0, i64 %i
  %p = load ptr addrspace(1), ptr %src, align 8
  %dst = getelementptr inbounds [2 x ptr addrspace(1)], ptr %copy, i64 0, i64 %i
  store ptr addrspace(1) %p, ptr %dst, align 8
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, 2
  br i1 %done, label %out, label %loop
out:
  %w = and i32 %t, 1
  %wi = zext i32 %w to i64
  %cp = getelementptr inbounds [2 x ptr addrspace(1)], ptr %copy, i64 0, i64 %wi
  %chosen = load ptr addrspace(1), ptr %cp, align 8
  %ti = zext i32 %t to i64
  %at = getelementptr inbounds i32, ptr addrspace(1) %chosen, i64 %ti
  %v = load i32, ptr addrspace(1) %at, align 4
  %t8 = add i64 %ti, 8
  %o = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %t8
  store i32 %v, ptr addrspace(1) %o, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @pt, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
