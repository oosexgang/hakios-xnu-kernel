; A pointer a loop carries from one buffer to another, as MPS's RNN kernels step through their inputs:
; the first iteration reads buffer 0 (zeroed), the second buffer 1 (1000 + i), and their sum is
; written -- checked by tools/air/wordcheck.sh: out[8 + t] = 1000 + t.
;
;     kernel void pl(device uint *out [[buffer(0)]], constant uint *in [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) {
;         const device uint *p = out; uint sum = 0;
;         for (uint i = 0; i < 2; i++) { sum += p[t]; p = in; }
;         out[8 + t] = sum; }
source_filename = "pl"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @pl(ptr addrspace(1) %0, ptr addrspace(2) %1, i32 %t) local_unnamed_addr {
  %in = addrspacecast ptr addrspace(2) %1 to ptr addrspace(1)
  %ti = zext i32 %t to i64
  br label %loop
loop:
  %i = phi i32 [ 0, %2 ], [ %next, %loop ]
  %p = phi ptr addrspace(1) [ %0, %2 ], [ %in, %loop ]
  %sum = phi i32 [ 0, %2 ], [ %s2, %loop ]
  %at = getelementptr inbounds i32, ptr addrspace(1) %p, i64 %ti
  %v = load i32, ptr addrspace(1) %at, align 4
  %s2 = add i32 %sum, %v
  %next = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %next, 2
  br i1 %done, label %out, label %loop
out:
  %t8 = add i64 %ti, 8
  %o = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %t8
  store i32 %s2, ptr addrspace(1) %o, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @pl, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
