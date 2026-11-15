; A memcpy whose length the shader reads, dispatched by tools/air/memcpycheck.c on one thread:
; n bytes from one buffer to the other, n from a constant buffer. 37 is nine words and a tail.
;
;     kernel void mc(device uchar *dst [[buffer(0)]], device const uchar *src [[buffer(1)]],
;                    constant uint &n [[buffer(2)]]) { memcpy(dst, src, n); }
source_filename = "mc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

define void @mc(ptr addrspace(1) %0, ptr addrspace(1) %1, ptr addrspace(2) %2) local_unnamed_addr {
  %n = load i32, ptr addrspace(2) %2, align 4
  %w = zext i32 %n to i64
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) align 4 %0, ptr addrspace(1) align 4 %1, i64 %w, i1 false)
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @mc, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 1, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uchar", !"air.arg_name", !"dst"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 1, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uchar", !"air.arg_name", !"src"}
!8 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"n"}
