; tex_fill_h, the AIR side: a HALF image written by a compute kernel, as the
; LLVM IR a Metal compiler would emit for
;
;     kernel void tex_fill_h(texture2d<half, access::write> out [[texture(0)]],
;                            uint2 gid                           [[thread_position_in_grid]])
;     {
;       out.write(half4(half(gid.x) / 64.0h, half(gid.y) / 32.0h, 0.5h, 1.0h), gid);
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata and of the intrinsic
; signature is Apple's convention (air.write_texture_2d.v4f16, as the image's
; libraries declare it).
;
; Why it exists: Apple's half kernels (MetalFaceMask_Clear and most of the
; image's texture kernels) store through texture2d<half, write>, and the front
; end carries a half as 16 bits the ir3 stib names as an integer type while the
; descriptor says FMT6_16_16_16_16_FLOAT. Whether the TP converts the integer
; or stores the bits is the question this kernel answers: every value below is
; exact in half (x/64 and y/32 for x < 64, y < 32; 0.5; 1.0), so the expected
; pixel is the four IEEE half bit patterns and nothing else.
source_filename = "tex_fill_h"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare void @air.write_texture_2d.v4f16(ptr addrspace(1), <2 x i32>, <4 x half>, i32, i32)

define void @tex_fill_h(ptr addrspace(1) %out, <2 x i32> %gid) local_unnamed_addr {
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %fx = uitofp i32 %gx to float
  %fy = uitofp i32 %gy to float
  %sx = fmul float %fx, 1.562500e-02
  %sy = fmul float %fy, 3.125000e-02
  %hx = fptrunc float %sx to half
  %hy = fptrunc float %sy to half
  %v0 = insertelement <4 x half> undef, half %hx, i32 0
  %v1 = insertelement <4 x half> %v0, half %hy, i32 1
  %v2 = insertelement <4 x half> %v1, half 0xH3800, i32 2
  %v3 = insertelement <4 x half> %v2, half 0xH3C00, i32 3
  tail call void @air.write_texture_2d.v4f16(ptr addrspace(1) %out, <2 x i32> %gid, <4 x half> %v3, i32 0, i32 0)
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_fill_h, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<half, write>", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
