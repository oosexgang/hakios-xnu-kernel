; Metal 3.2's atomic wait and notify and the device-coherent load and store, as MPSCore's
; single_pass_scan uses them to hand partial sums from one threadgroup to the next. Thread t waits
; for word t of buffer 1 (1000 + t) to be anything but 0 -- which it already is, so the wait
; returns at once with it --, adds the coherent read of word t + 8 and stores that, coherently,
; to out[t]; then notifies and yields, which are nothing to compute. Checked by
; tools/air/wordcheck.sh: out[t] is (1000 + t) + (1008 + t).
source_filename = "wn"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare i32 @air.atomic.global.wait.simdgroup.i32(ptr addrspace(1) nocapture, i32, i32, i32, i32, i1)
declare void @air.atomic.global.notify.one.simdgroup.i32(ptr addrspace(1) nocapture, i1)
declare i32 @air.load.device_coherent.i32.p1i32(ptr addrspace(1) nocapture readonly)
declare void @air.store.device_coherent.i32.p1i32(i32, ptr addrspace(1) nocapture writeonly)
declare void @llvm.agx3.yield(i16)

define void @wn(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %t) local_unnamed_addr {
  %tw = zext i32 %t to i64
  %p = getelementptr inbounds i32, ptr addrspace(1) %1, i64 %tw
  %w = tail call i32 @air.atomic.global.wait.simdgroup.i32(ptr addrspace(1) %p, i32 0, i32 2, i32 2, i32 1, i1 false)
  %t8 = add i64 %tw, 8
  %q = getelementptr inbounds i32, ptr addrspace(1) %1, i64 %t8
  %c = tail call i32 @air.load.device_coherent.i32.p1i32(ptr addrspace(1) %q)
  %s = add i32 %w, %c
  %o = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %tw
  tail call void @air.store.device_coherent.i32.p1i32(i32 %s, ptr addrspace(1) %o)
  tail call void @air.atomic.global.notify.one.simdgroup.i32(ptr addrspace(1) %p, i1 false)
  tail call void @llvm.agx3.yield(i16 0)
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 2, i32 0}
!3 = !{ptr @wn, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
