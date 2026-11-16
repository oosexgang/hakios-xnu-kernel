; An explicit imageblock in a kernel, dispatched by tools/air/imageblockcheck.c as one 4 x 4
; group: each thread puts (x, y, 7, 1) into its own pixel's data, and after a barrier thread
; (0, 0) writes the block's 4 x 4 pixels into an RGBA32Float texture at (2, 1), the way
; QuartzCore's compute blurs hand their result over.
;
;     struct Px { float4 c; };
;     kernel void ib(imageblock<Px, imageblock_layout_explicit> block,
;                    texture2d<float, access::write> t [[texture(0)]],
;                    ushort2 tid [[thread_position_in_threadgroup]]) {
;         block.data(tid)->c = float4(tid.x, tid.y, 7, 1);
;         threadgroup_barrier(mem_flags::mem_threadgroup_imageblock);
;         if (tid.x == 0 && tid.y == 0) t.write(block.slice(block.data(0)->c), ushort2(2, 1));
;     }
source_filename = "ib"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }

declare ptr addrspace(4) @air.imageblock_data(<2 x i16>, i32, i16)
declare void @air.wg.barrier(i32, i32, i32)
declare void @air.write_imageblock_slice_to_texture_2d.i16.v4f32(ptr addrspace(1), ptr addrspace(4), i1, <2 x i16>, <2 x i16>, <2 x i16>, i16, i1, i32)

define void @ib(%"struct.metal::_imageblock_base" %0, ptr addrspace(1) %1, <2 x i16> %2) local_unnamed_addr {
  %d = call ptr addrspace(4) @air.imageblock_data(<2 x i16> %2, i32 0, i16 0)
  %x = extractelement <2 x i16> %2, i64 0
  %y = extractelement <2 x i16> %2, i64 1
  %xf = uitofp i16 %x to float
  %yf = uitofp i16 %y to float
  %c0 = insertelement <4 x float> <float 0.0, float 0.0, float 7.0, float 1.0>, float %xf, i64 0
  %c1 = insertelement <4 x float> %c0, float %yf, i64 1
  store <4 x float> %c1, ptr addrspace(4) %d, align 16
  call void @air.wg.barrier(i32 8, i32 5, i32 1)
  %o = or i16 %x, %y
  %first = icmp eq i16 %o, 0
  br i1 %first, label %w, label %e
w:
  %s = call ptr addrspace(4) @air.imageblock_data(<2 x i16> zeroinitializer, i32 0, i16 0)
  call void @air.write_imageblock_slice_to_texture_2d.i16.v4f32(ptr addrspace(1) %1, ptr addrspace(4) %s, i1 true, <2 x i16> zeroinitializer, <2 x i16> <i16 4, i16 4>, <2 x i16> <i16 2, i16 1>, i16 0, i1 false, i32 2)
  br label %e
e:
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 1, i32 0}
!3 = !{ptr @ib, !4, !5}
!4 = !{}
!5 = !{!6, !8, !9}
!6 = !{i32 0, !"air.imageblock", !"explicit", !"air.imageblock_data_size", i32 16, !"air.struct_type_info", !7, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"imageblock<Px, layout_explicit>", !"air.arg_name", !"block"}
!7 = !{i32 0, i32 16, i32 0, !"float4", !"c"}
!8 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"t"}
!9 = !{i32 2, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tid"}
