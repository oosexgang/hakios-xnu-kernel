; tools/air/tgcheck.sh: a kernel with threadgroup memory of its own (@own) and a threadgroup buffer sized at
; dispatch (%1), which must not be the same bytes
source_filename = "tg"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

@own = internal addrspace(3) global [16 x i32] undef

declare void @air.wg.barrier(i32, i32) local_unnamed_addr

; every thread writes its index + 100 to its slot of the kernel's own array and its index + 1000 to
; its slot of the buffer sized at dispatch, then reads both back from the slot of the thread after it
define void @tg(ptr addrspace(1) %0, ptr addrspace(3) %1, i32 %2) local_unnamed_addr {
  %w = zext i32 %2 to i64
  %o = getelementptr inbounds [16 x i32], ptr addrspace(3) @own, i64 0, i64 %w
  %a = add i32 %2, 100
  store i32 %a, ptr addrspace(3) %o, align 4
  %d = getelementptr inbounds i32, ptr addrspace(3) %1, i64 %w
  %b = add i32 %2, 1000
  store i32 %b, ptr addrspace(3) %d, align 4
  call void @air.wg.barrier(i32 2, i32 1)
  %n1 = add i32 %2, 1
  %n = and i32 %n1, 15
  %nw = zext i32 %n to i64
  %o2 = getelementptr inbounds [16 x i32], ptr addrspace(3) @own, i64 0, i64 %nw
  %d2 = getelementptr inbounds i32, ptr addrspace(3) %1, i64 %nw
  %x = load i32, ptr addrspace(3) %o2, align 4
  %y = load i32, ptr addrspace(3) %d2, align 4
  %w2 = mul i64 %w, 2
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %w2
  store i32 %x, ptr addrspace(1) %p0, align 4
  %w3 = add i64 %w2, 1
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %w3
  store i32 %y, ptr addrspace(1) %p1, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @tg, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 128, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"dyn"}
!8 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
