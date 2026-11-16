; tri_null_fs, Metal's null texture sampled as 2D and as 3D through the one slot:
; the LLVM IR a Metal compiler would emit for
;
;     struct Vary { float4 position [[position]]; float4 color; };
;     fragment float4 tri_null_fs(Vary in [[stage_in]], sampler s [[sampler(0)]])
;     {
;       float4 a = texture2d<float>(/* null */).sample(s, in.color.xy);
;       float4 b = texture3d<float>(/* null */).sample(s, float3(in.color.xy, 0.5));
;       return float4(a.x, a.y, b.x, b.y);   // [[color(0)]]
;     }
;
; which is the shape of QuartzCore's YCbCr uber shaders, whose one reserved slot
; the front end makes of air.get_null_texture_2d and _3d alike (nir_air.h:
; air_resource.null_texture). The slot has one descriptor and the draw binds a
; 2D 1x1 texture there; the question this answers is whether a 3D sample through
; that descriptor reads its texel. The probe writes the texel with a value that
; is not zero, so a sample that reads nothing (a zero) and one that reads the
; texel come out different: red and green are the 2D sample's, blue and alpha
; the 3D one's, and all four are the texel's red and green when both read it.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata and of the intrinsic signatures is Apple's convention (tri_tex_fs.ll).

source_filename = "tri_null_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare ptr addrspace(1) @air.get_null_texture_2d()
declare ptr addrspace(1) @air.get_null_texture_3d()
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i1, <3 x i32>, i1, float, float, i32)

define <4 x float> @tri_null_fs(<4 x float> %color, ptr addrspace(2) %s) local_unnamed_addr {
  %t2 = tail call ptr addrspace(1) @air.get_null_texture_2d()
  %t3 = tail call ptr addrspace(1) @air.get_null_texture_3d()
  %uv = shufflevector <4 x float> %color, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %uvw = shufflevector <4 x float> %color, <4 x float> <float 0.0, float 0.0, float 0.5, float 0.0>, <3 x i32> <i32 0, i32 1, i32 6>
  %ra = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t2, ptr addrspace(2) %s, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %rb = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) %t3, ptr addrspace(2) %s, <3 x float> %uvw, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %a = extractvalue { <4 x float>, i8 } %ra, 0
  %b = extractvalue { <4 x float>, i8 } %rb, 0
  %v = shufflevector <4 x float> %a, <4 x float> %b, <4 x i32> <i32 0, i32 1, i32 4, i32 5>
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_null_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !8}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!8 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
