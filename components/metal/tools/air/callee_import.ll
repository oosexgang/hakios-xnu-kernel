; Values of a caller a callee reads without taking them as arguments, dispatched by
; tools/air/importcheck.c: a struct holding a pointer at an offset the caller computed, passed
; whole (MPS's kernels hand their helpers such structs), and a pointer the caller stored into a
; local whose address it passes. Thread t writes 3t through the first and 5t + 1 through the
; second, each at the offset the caller computed, so a callee that read either as undefined
; writes somewhere else or nothing.
;
;     struct Out { device uint *p; };
;     static void f(Out o, uint t) { *o.p = 3 * t; }
;     static void g(thread device uint **q, uint t) { **q = 5 * t + 1; }
;     kernel void imp(device uint *out [[buffer(0)]], uint t [[thread_position_in_grid]]) {
;         Out o = { out + 2 * t };  f(o, t);
;         device uint *p = out + 2 * t + 1;  g(&p, t);
;     }
source_filename = "imp"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%struct.Out = type { ptr addrspace(1) }

define internal void @f(%struct.Out %o, i32 %t) {
  %p = extractvalue %struct.Out %o, 0
  %v = mul i32 %t, 3
  store i32 %v, ptr addrspace(1) %p, align 4
  ret void
}

define internal void @g(ptr %q, i32 %t) {
  %p = load ptr addrspace(1), ptr %q, align 8
  %v0 = mul i32 %t, 5
  %v = add i32 %v0, 1
  store i32 %v, ptr addrspace(1) %p, align 4
  ret void
}

define void @imp(ptr addrspace(1) %0, i32 %1) local_unnamed_addr {
  %slot = alloca ptr addrspace(1), align 8
  %w = zext i32 %1 to i64
  %i0 = mul i64 %w, 2
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %i0
  %o = insertvalue %struct.Out undef, ptr addrspace(1) %p0, 0
  call void @f(%struct.Out %o, i32 %1)
  %i1 = add i64 %i0, 1
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %i1
  store ptr addrspace(1) %p1, ptr %slot, align 8
  call void @g(ptr %slot, i32 %1)
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @imp, !4, !5}
!4 = !{}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 128, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
