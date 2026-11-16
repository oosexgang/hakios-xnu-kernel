; A kernel that writes one texture at two mip levels, dispatched by tools/air/mipwritecheck.c on
; a 4 x 4 group over a 4 x 4 RGBA32Float texture with two levels: every thread writes (x, y, 0, 1)
; into level 0, and the four of the top left quarter write (x, y, 1, 1) into level 1 -- the way a
; mipmap kernel (CoreRE's) fills a chain. The two levels need two image views.
;
;     kernel void mw(texture2d<float, access::write> t [[texture(0)]],
;                    uint2 p [[thread_position_in_grid]]) {
;         t.write(float4(p.x, p.y, 0, 1), p, 0);
;         if (p.x < 2 && p.y < 2) t.write(float4(p.x, p.y, 1, 1), p, 1);
;     }
source_filename = "mw"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32, i32)

define void @mw(ptr addrspace(1) %0, <2 x i32> %1) local_unnamed_addr {
  %x = extractelement <2 x i32> %1, i64 0
  %y = extractelement <2 x i32> %1, i64 1
  %xf = uitofp i32 %x to float
  %yf = uitofp i32 %y to float
  %a0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %xf, i64 0
  %a1 = insertelement <4 x float> %a0, float %yf, i64 1
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %a1, i32 0, i32 2)
  %o = or i32 %x, %y
  %small = icmp ult i32 %o, 2
  br i1 %small, label %w, label %e
w:
  %b = insertelement <4 x float> %a1, float 1.0, i64 2
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %b, i32 1, i32 2)
  br label %e
e:
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 1, i32 0}
!3 = !{ptr @mw, !4, !5}
!4 = !{}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"t"}
!7 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"p"}
