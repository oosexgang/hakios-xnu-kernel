; Two buffer arguments compared, the way MPS works in place when its source is its destination,
; dispatched by tools/air/bufcmpcheck.c on one thread:
;
;     kernel void bc(device uint *dst [[buffer(0)]], device const uint *src [[buffer(1)]]) {
;         dst[0] = dst == src ? 1 : 2;  dst[1] = dst + 1 == src ? 3 : 4; }

source_filename = "bc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @bc(ptr addrspace(1) %0, ptr addrspace(1) %1) local_unnamed_addr {
  %same = icmp eq ptr addrspace(1) %0, %1
  %v = select i1 %same, i32 1, i32 2
  store i32 %v, ptr addrspace(1) %0, align 4
  %d1 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 1
  %next = icmp ne ptr addrspace(1) %d1, %1
  %w = select i1 %next, i32 4, i32 3
  store i32 %w, ptr addrspace(1) %d1, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @bc, !4, !5}
!4 = !{}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
