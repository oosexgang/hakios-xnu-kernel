; tex_fill_3d, the AIR side: a 3D image written by a compute kernel, one slice a grid slice, as the LLVM IR a Metal compiler would emit for
;
; kernel void tex_fill_3d(texture3d<uint, access::write> out [[texture(0)]],
;                         uint3 gid [[thread_position_in_grid]])
; {
;   out.write(uint4(gid.x, gid.y, gid.z, 0xdead), gid);
; }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata and of the intrinsic
; signature is Apple's convention.
;
; Every texel gets its own (x, y, layer) triple and 0xdead, so a layer stored
; at the wrong pitch, or every layer landing on the first, leaves texels that
; are canary or name another layer -- not one plausible-looking fill.
source_filename = "tex_fill_3d"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare void @air.write_texture_3d.u.v4i32(ptr addrspace(1), <3 x i32>, <4 x i32>, i32, i32)

define void @tex_fill_3d(ptr addrspace(1) %out, <3 x i32> %gid) local_unnamed_addr {
  %gx = extractelement <3 x i32> %gid, i32 0
  %gy = extractelement <3 x i32> %gid, i32 1
  %gz = extractelement <3 x i32> %gid, i32 2
  %xy0 = insertelement <2 x i32> undef, i32 %gx, i32 0
  %xy = insertelement <2 x i32> %xy0, i32 %gy, i32 1
  %v0 = insertelement <4 x i32> undef, i32 %gx, i32 0
  %v1 = insertelement <4 x i32> %v0, i32 %gy, i32 1
  %v2 = insertelement <4 x i32> %v1, i32 %gz, i32 2
  %v3 = insertelement <4 x i32> %v2, i32 57005, i32 3
  tail call void @air.write_texture_3d.u.v4i32(ptr addrspace(1) %out, <3 x i32> %gid, <4 x i32> %v3, i32 0, i32 0)
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_fill_3d, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture3d<uint, write>", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
