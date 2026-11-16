; air.get_imageblock_width and _height, which Portrait's backgroundReplacement writes its block out by:
; the block is the threadgroup's, a pixel a thread -- checked by tools/air/wordcheck.sh on a group of
; eight threads, 8 by 1.
;
;     kernel void ibs(device uint *out [[buffer(0)]]) {
;         out[0] = get_imageblock_width(); out[1] = get_imageblock_height(); }
source_filename = "ibs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare i16 @air.get_imageblock_width()
declare i16 @air.get_imageblock_height()

define void @ibs(ptr addrspace(1) %0) local_unnamed_addr {
  %w = call i16 @air.get_imageblock_width()
  %h = call i16 @air.get_imageblock_height()
  %w32 = zext i16 %w to i32
  %h32 = zext i16 %h to i32
  store i32 %w32, ptr addrspace(1) %0, align 4
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 1
  store i32 %h32, ptr addrspace(1) %p1, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ibs, !4, !5}
!4 = !{}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
