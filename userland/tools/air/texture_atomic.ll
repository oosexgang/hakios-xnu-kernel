; Texture atomics, dispatched by tools/air/texatomcheck.c: sixteen threads each take the max of
; texel (0, 0) of an R32Uint texture with their index and add one to texel (1, 0), so the two
; must end 15 and 16 whatever order the threads ran in, and each thread writes what the add
; handed back into a buffer, which must be 0..15 in some order.
;
;     kernel void ta(texture2d<uint, access::read_write> t [[texture(0)]],
;                    device uint *was [[buffer(0)]], uint i [[thread_position_in_grid]]) {
;         t.atomic_fetch_max(uint2(0, 0), i);
;         was[i] = t.atomic_fetch_add(uint2(1, 0), 1).x;
;     }
source_filename = "ta"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare <4 x i32> @air.atomic_fetch_max_explicit_texture_2d.i16.u.v4i32(ptr addrspace(1), <2 x i16>, <2 x i16>, <4 x i32>, i32, i32)
declare <4 x i32> @air.atomic_fetch_add_explicit_texture_2d.i16.u.v4i32(ptr addrspace(1), <2 x i16>, <2 x i16>, <4 x i32>, i32, i32)

define void @ta(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %v = insertelement <4 x i32> zeroinitializer, i32 %2, i64 0
  %m = call <4 x i32> @air.atomic_fetch_max_explicit_texture_2d.i16.u.v4i32(ptr addrspace(1) %0, <2 x i16> zeroinitializer, <2 x i16> zeroinitializer, <4 x i32> %v, i32 0, i32 3)
  %a = call <4 x i32> @air.atomic_fetch_add_explicit_texture_2d.i16.u.v4i32(ptr addrspace(1) %0, <2 x i16> <i16 1, i16 0>, <2 x i16> zeroinitializer, <4 x i32> <i32 1, i32 0, i32 0, i32 0>, i32 0, i32 3)
  %x = extractelement <4 x i32> %a, i64 0
  %w = zext i32 %2 to i64
  %p = getelementptr inbounds i32, ptr addrspace(1) %1, i64 %w
  store i32 %x, ptr addrspace(1) %p, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 1, i32 0}
!3 = !{ptr @ta, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_name", !"texture2d<uint, read_write>", !"air.arg_name", !"t"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"was"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
