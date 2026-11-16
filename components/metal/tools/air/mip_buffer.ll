; mip_write.ll with the levels read out of a constant buffer, as CoreAnimation's variable blur
; copies its base level: the level is the ushort its uniforms hold (1 here), and the kernel writes
; at that level and at the one below it (`add i16 %level, -1`). Dispatched by
; tools/air/mipwritecheck.c (`buffer`): every thread writes (x, y, 0, 1) at level - 1, and the
; four of the top left quarter (x, y, 1, 1) at level.
;
;     kernel void mb(texture2d<float, access::write> t [[texture(0)]],
;                    constant ushort &level [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
;         t.write(float4(p.x, p.y, 0, 1), p, level - 1);
;         if (p.x < 2 && p.y < 2) t.write(float4(p.x, p.y, 1, 1), p, level);
;     }
source_filename = "mb"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i16, i32)

define void @mb(ptr addrspace(1) %0, ptr addrspace(2) %u, <2 x i32> %1) local_unnamed_addr {
  %lv = load i16, ptr addrspace(2) %u, align 2
  %below = add i16 %lv, -1
  %x = extractelement <2 x i32> %1, i64 0
  %y = extractelement <2 x i32> %1, i64 1
  %xf = uitofp i32 %x to float
  %yf = uitofp i32 %y to float
  %a0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %xf, i64 0
  %a1 = insertelement <4 x float> %a0, float %yf, i64 1
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %a1, i16 %below, i32 2)
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
!3 = !{ptr @mb, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"t"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 2, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"ushort", !"air.arg_name", !"level"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"p"}
