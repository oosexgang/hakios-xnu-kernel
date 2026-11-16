; tg_arg, the AIR side: a static threadgroup array beside a threadgroup argument
; the encoder sizes, as the LLVM IR a Metal compiler would emit for
;
;     kernel void tg_arg(device uint* out [[buffer(0)]], threadgroup uint* t [[threadgroup(0)]],
;                        uint lid [[thread_position_in_threadgroup]], uint gid [[thread_position_in_grid]])
;     {
;       threadgroup uint s[64];
;       s[lid] = lid + 1000;
;       t[lid * 32] = lid + 2000;
;       threadgroup_barrier(mem_flags::mem_threadgroup);
;       uint n = (lid + 1) & 63;
;       out[2 * gid] = s[n];
;       out[2 * gid + 1] = t[n * 32];
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata and of the intrinsic
; signature is Apple's convention.
;
; Why it exists: the argument has to start past s, which the front end lays out
; from offset 0 -- at 0 the two share their first bytes and s[n] reads t's word.
; t's words lie 128 bytes apart, so 64 threads reach 8 KiB into it: the dispatch
; has to be given the encoder's length, not the 2 KiB floor, or the far ones are
; lost. Each word read is a neighbour's, so it is written by another thread and
; the barrier is what makes it there.
source_filename = "tg_arg"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@s = internal addrspace(3) global [64 x i32] undef, align 4

declare void @air.wg.barrier(i32, i32)

define void @tg_arg(ptr addrspace(1) %out, ptr addrspace(3) %t, i32 %lid, i32 %gid) local_unnamed_addr {
  %sv = add i32 %lid, 1000
  %sp = getelementptr inbounds [64 x i32], ptr addrspace(3) @s, i32 0, i32 %lid
  store i32 %sv, ptr addrspace(3) %sp, align 4
  %tv = add i32 %lid, 2000
  %ti = mul i32 %lid, 32
  %tp = getelementptr inbounds i32, ptr addrspace(3) %t, i32 %ti
  store i32 %tv, ptr addrspace(3) %tp, align 4
  tail call void @air.wg.barrier(i32 2, i32 1)
  %l1 = add i32 %lid, 1
  %n = and i32 %l1, 63
  %sq = getelementptr inbounds [64 x i32], ptr addrspace(3) @s, i32 0, i32 %n
  %sr = load i32, ptr addrspace(3) %sq, align 4
  %ni = mul i32 %n, 32
  %tq = getelementptr inbounds i32, ptr addrspace(3) %t, i32 %ni
  %tr = load i32, ptr addrspace(3) %tq, align 4
  %o0 = shl i32 %gid, 1
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %o0
  store i32 %sr, ptr addrspace(1) %p0, align 4
  %o1 = add i32 %o0, 1
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %o1
  store i32 %tr, ptr addrspace(1) %p1, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tg_arg, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
!7 = !{i32 2, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
