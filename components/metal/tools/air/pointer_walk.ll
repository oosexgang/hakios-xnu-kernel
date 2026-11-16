; A loop that walks a pointer through a local array up to its end pointer, as Morpheus clears
; its accumulators, dispatched by tools/air/bufcmpcheck.c on one thread: 5, 6, 7, 8 written and
; their sum, 26, stored at dst[2].
;
;     kernel void walk(device uint *dst [[buffer(0)]]) {
;         uint a[4], v = 5; for (uint *p = a; p != a + 4; p++) *p = v++;
;         dst[2] = a[0] + a[1] + a[2] + a[3]; }
source_filename = "walk"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @walk(ptr addrspace(1) %0) local_unnamed_addr {
  %a = alloca [4 x i32], align 4
  %end = getelementptr inbounds [4 x i32], ptr %a, i64 1
  br label %loop
loop:
  %p = phi ptr [ %a, %1 ], [ %q, %loop ]
  %v = phi i32 [ 5, %1 ], [ %w, %loop ]
  store i32 %v, ptr %p, align 4
  %q = getelementptr inbounds i32, ptr %p, i64 1
  %w = add i32 %v, 1
  %done = icmp eq ptr %q, %end
  br i1 %done, label %out, label %loop
out:
  %x0 = load i32, ptr %a, align 4
  %a1 = getelementptr inbounds i32, ptr %a, i64 1
  %x1 = load i32, ptr %a1, align 4
  %a2 = getelementptr inbounds i32, ptr %a, i64 2
  %x2 = load i32, ptr %a2, align 4
  %a3 = getelementptr inbounds i32, ptr %a, i64 3
  %x3 = load i32, ptr %a3, align 4
  %s1 = add i32 %x0, %x1
  %s2 = add i32 %s1, %x2
  %s3 = add i32 %s2, %x3
  %d2 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 2
  store i32 %s3, ptr addrspace(1) %d2, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @walk, !4, !5}
!4 = !{}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
