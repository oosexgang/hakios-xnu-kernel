; mip_write.ll with the second level computed from a float a constant buffer holds, as VFX's
; compute_bloom_down_sample_lod writes at (ushort)(its uniform's level + 1.0). Dispatched by
; tools/air/mipwritecheck.c (`float`, the buffer holding 0.0): every thread writes (x, y, 0, 1) at
; level 0, and the four of the top left quarter (x, y, 1, 1) at (ushort)(0.0 + 1.0), level 1.
;
;     kernel void mf(texture2d<float, access::write> t [[texture(0)]],
;                    constant float &lod [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
;         t.write(float4(p.x, p.y, 0, 1), p, 0);
;         if (p.x < 2 && p.y < 2) t.write(float4(p.x, p.y, 1, 1), p, ushort(lod + 1.0f));
;     }
source_filename = "mf"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i16, i32)
declare i16 @air.convert.u.i16.f.f32(float)

define void @mf(ptr addrspace(1) %0, ptr addrspace(2) %u, <2 x i32> %1) local_unnamed_addr {
  %f = load float, ptr addrspace(2) %u, align 4
  %f1 = fadd fast float %f, 1.0
  %lv = tail call i16 @air.convert.u.i16.f.f32(float %f1)
  %x = extractelement <2 x i32> %1, i64 0
  %y = extractelement <2 x i32> %1, i64 1
  %xf = uitofp i32 %x to float
  %yf = uitofp i32 %y to float
  %a0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %xf, i64 0
  %a1 = insertelement <4 x float> %a0, float %yf, i64 1
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %a1, i16 0, i32 2)
  %o = or i32 %x, %y
  %small = icmp ult i32 %o, 2
  br i1 %small, label %w, label %e
w:
  %b = insertelement <4 x float> %a1, float 1.0, i64 2
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %b, i16 %lv, i32 2)
  br label %e
e:
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 1, i32 0}
!3 = !{ptr @mf, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"t"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"lod"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"p"}
