; tex_sample_wide, the AIR side: tex_sample_gid with the coordinate spread over
; [-1, 2] on each axis, as the LLVM IR a Metal compiler would emit for
;     kernel void tex_sample_wide(texture2d<float, access::sample> in [[texture(0)]],
;                                 sampler s                           [[sampler(0)]],
;                                 device float4 *out                  [[buffer(0)]],
;                                 uint2 gid                           [[thread_position_in_grid]],
;                                 uint2 grid                          [[threads_per_grid]])
;     {
;       out[gid.y * grid.x + gid.x] = in.sample(s, (float2(gid) + 0.5) / float2(grid) * 3.0 - 1.0);
;     }
; Hand-written, ours, MIT: only the SHAPE of the metadata and of the intrinsic
; signature is Apple's convention.
;
; Why it exists: two thirds of these coordinates are outside the texture, which
; is where a sampler's address mode and border colour decide the texel -- the
; probe holds each mode against its own rule and against the others'.
source_filename = "tex_sample_wide"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define void @tex_sample_wide(ptr addrspace(1) %in, ptr addrspace(2) %s, ptr addrspace(1) %out, <2 x i32> %gid, <2 x i32> %grid) local_unnamed_addr {
  %gf = uitofp <2 x i32> %gid to <2 x float>
  %gc = fadd <2 x float> %gf, <float 5.000000e-01, float 5.000000e-01>
  %nf = uitofp <2 x i32> %grid to <2 x float>
  %c0 = fdiv <2 x float> %gc, %nf
  %c3 = fmul <2 x float> %c0, <float 3.000000e+00, float 3.000000e+00>
  %c = fsub <2 x float> %c3, <float 1.000000e+00, float 1.000000e+00>
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %in, ptr addrspace(2) %s, <2 x float> %c, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %nx = extractelement <2 x i32> %grid, i32 0
  %row = mul i32 %gy, %nx
  %idx = add i32 %row, %gx
  %t = zext i32 %idx to i64
  %po = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %t
  store <4 x float> %v, ptr addrspace(1) %po, align 16
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_sample_wide, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"in"}
!6 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!9 = !{i32 4, !"air.threads_per_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"grid"}
