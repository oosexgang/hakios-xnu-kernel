; tri_cube_fs, a fragment function that samples a cube along a direction from a constant buffer: the LLVM IR a Metal
; compiler would emit for
;
;     fragment float4 tri_cube_fs(constant float4 &dir [[buffer(0)]], texturecube<float> t [[texture(0)]],
;                                 sampler s [[sampler(0)]])
;     {
;       return t.sample(s, dir.xyz);
;     }
;
; Every pixel samples the one texel the direction points at; with each face of the cube one colour, the whole target is
; that face's colour, and the face is the direction's major axis (+X, -X, +Y, -Y, +Z, -Z: Metal's face order).
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_cube_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i1, float, float, i32)

define <4 x float> @tri_cube_fs(ptr addrspace(2) %c, ptr addrspace(1) %t, ptr addrspace(2) %s) local_unnamed_addr {
  %d = load <4 x float>, ptr addrspace(2) %c, align 16
  %xyz = shufflevector <4 x float> %d, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %r = tail call { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <3 x float> %xyz, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_cube_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texturecube<float, sample>", !"air.arg_name", !"t"}
!8 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
