; A pointer through an integer and back, as MPSImage's DilateFilter4 and ErodeFilter4 find their
; run-length rows: `inttoptr (ptrtoint %in + bytes)`. Thread t reads word t + 2 of buffer 1 (which
; holds 1000 + i) through an integer sum and a difference -- checked by tools/air/wordcheck.sh:
; out[t] is 1002 + t.
;
;     kernel void pi(device uint *out [[buffer(0)]], constant uint *in [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) {
;         ulong a = (ulong)in + 4 * t + 12 - 4;
;         out[t] = *(constant uint *)a;
;     }
source_filename = "pi"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @pi(ptr addrspace(1) %0, ptr addrspace(2) %1, i32 %t) local_unnamed_addr {
  %base = ptrtoint ptr addrspace(2) %1 to i64
  %tw = zext i32 %t to i64
  %bytes = shl i64 %tw, 2
  %a0 = add i64 %bytes, %base
  %a1 = add i64 %a0, 12
  %a2 = sub i64 %a1, 4
  %p = inttoptr i64 %a2 to ptr addrspace(2)
  %v = load i32, ptr addrspace(2) %p, align 4
  %q = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %tw
  store i32 %v, ptr addrspace(1) %q, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @pi, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
