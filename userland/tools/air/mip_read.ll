; A read_write texture read and written at the level a constant buffer holds (1 here), the way VFX's
; vfx_reprojection_map works on its map at the level its uniforms say. Dispatched by
; tools/air/mipwritecheck.c (`read`) over a texture whose level 0 holds (x, y, 0, 1) and level 1
; (x, y, 1, 1): the four threads of the top left quarter read their texel at the level, add 10 to
; its blue and write it back there, so level 1 ends as (x, y, 11, 1) and level 0 is left as it was.
;
;     kernel void mr(texture2d<float, access::read_write> t [[texture(0)]],
;                    constant ushort &level [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
;         if (p.x < 2 && p.y < 2) t.write(t.read(p, level) + float4(0, 0, 10, 0), p, level);
;     }
source_filename = "mr"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i16, i32)

define void @mr(ptr addrspace(1) %0, ptr addrspace(2) %u, <2 x i32> %1) local_unnamed_addr {
  %lv = load i16, ptr addrspace(2) %u, align 2
  %x = extractelement <2 x i32> %1, i64 0
  %y = extractelement <2 x i32> %1, i64 1
  %o = or i32 %x, %y
  %small = icmp ult i32 %o, 2
  br i1 %small, label %w, label %e
w:
  %lw = zext i16 %lv to i32
  %rs = tail call ptr addrspace(2) @air.get_read_sampler()
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %0, ptr addrspace(2) %rs, <2 x i32> %1, <2 x i32> zeroinitializer, i32 %lw, i32 3)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %b = fadd <4 x float> %v, <float 0.0, float 0.0, float 10.0, float 0.0>
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %0, <2 x i32> %1, <4 x float> %b, i16 %lv, i32 3)
  br label %e
e:
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 1, i32 0}
!3 = !{ptr @mr, !4, !5}
!4 = !{}
!5 = !{!6, !8, !7}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_name", !"texture2d<float, read_write>", !"air.arg_name", !"t"}
!8 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 2, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"ushort", !"air.arg_name", !"level"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"p"}
