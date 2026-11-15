; An atomic through a pointer chosen at run time, as Vista's splat culling counts into one of two
; buffers it loads out of its argument buffer: even threads add to word 0 of buffer 0, odd ones to
; word 0 of buffer 1 (which holds 1000 + i), and each keeps whether the old value it got back came
; from buffer 1 -- checked by tools/air/wordcheck.sh: out[0] is 4, out[8 + t] is 2 for even t and 1
; for odd t.
;
;     kernel void pa(device atomic_uint *out [[buffer(0)]], device atomic_uint *in [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) {
;         device atomic_uint *p = t & 1 ? in : out;
;         uint old = atomic_fetch_add_explicit(p, 1, memory_order_relaxed);
;         ((device uint *)out)[8 + t] = old >= 1000 ? 1 : 2;
;     }
source_filename = "pa"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1) nocapture, i32, i32, i32, i32, i1)

define void @pa(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %t) local_unnamed_addr {
  %odd = and i32 %t, 1
  %isodd = icmp ne i32 %odd, 0
  %p = select i1 %isodd, ptr addrspace(1) %1, ptr addrspace(1) %0
  %old = tail call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p, i32 1, i32 0, i32 2, i32 0, i1 true)
  %big = icmp uge i32 %old, 1000
  %v = select i1 %big, i32 1, i32 2
  %i = add i32 %t, 8
  %iw = zext i32 %i to i64
  %q = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %iw
  store i32 %v, ptr addrspace(1) %q, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @pa, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
