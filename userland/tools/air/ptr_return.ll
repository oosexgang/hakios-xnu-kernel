; A function that hands back a pointer, as RealityKit's povcGetCrWorldToProjMatrix hands back one into
; its constant buffers: pick returns buffer 0 for even t and buffer 1 (1000 + i) for odd t, and the
; kernel reads word t through it -- checked by tools/air/wordcheck.sh: out[8 + t] is 0 for even t and
; 1000 + t for odd t.
;
;     static const device uint *pick(const device uint *a, const device uint *b, uint t) {
;         return t & 1 ? b : a; }
;     kernel void pr(device uint *out [[buffer(0)]], constant uint *in [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) { out[8 + t] = pick(out, in, t)[t]; }
source_filename = "pr"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define internal ptr addrspace(1) @pick(ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %t) noinline {
  %odd = and i32 %t, 1
  %c = icmp ne i32 %odd, 0
  %p = select i1 %c, ptr addrspace(1) %b, ptr addrspace(1) %a
  ret ptr addrspace(1) %p
}

define void @pr(ptr addrspace(1) %0, ptr addrspace(2) %1, i32 %t) local_unnamed_addr {
  %in = addrspacecast ptr addrspace(2) %1 to ptr addrspace(1)
  %p = call ptr addrspace(1) @pick(ptr addrspace(1) %0, ptr addrspace(1) %in, i32 %t)
  %ti = zext i32 %t to i64
  %at = getelementptr inbounds i32, ptr addrspace(1) %p, i64 %ti
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
!3 = !{ptr @pr, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
