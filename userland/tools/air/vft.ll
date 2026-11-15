; A kernel that calls a function out of a visible function table, as CoreImage's ubershader does,
; dispatched by tools/air/vftcheck.c with tools/air/vft_add.ll and tools/air/vft_mul.ll linked: the
; table at buffer 1 names add1 for even entries and mul10 for odd ones, and thread t writes
; table[t & 1](t).
;
;     kernel void vt(device uint *out [[buffer(0)]],
;                    visible_function_table<uint(uint)> table [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) { out[t] = table[t & 1](t); }
source_filename = "vt"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)

define void @vt(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %t) local_unnamed_addr {
  %w = and i32 %t, 1
  %fp = call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %1, i32 %w)
  %v = call i32 %fp(i32 %t)
  %ti = zext i32 %t to i64
  %o = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %ti
  store i32 %v, ptr addrspace(1) %o, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @vt, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.visible_function_table", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"visible_function_table", !"air.arg_name", !"table"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
