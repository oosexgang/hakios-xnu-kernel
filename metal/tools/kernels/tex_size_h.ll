; tex_size_h, the AIR side: a half texture sampled and then asked its size, as
; the LLVM IR a Metal compiler would emit for
;
;     kernel void tex_size_h(texture2d<half, access::sample> in  [[texture(0)]],
;                            sampler                         s   [[sampler(0)]],
;                            texture2d<half, access::write>  out [[texture(1)]],
;                            uint2 gid                           [[thread_position_in_grid]])
;     {
;       half4 c = in.sample(s, float2(gid) / 64.0);
;       out.write(c * half(in.get_width()), gid);
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata and of the intrinsic
; signatures is Apple's convention.
;
; Why it exists: a size query is a tex instruction too (txs), and its result is
; an integer whatever the texture holds. A walk that took the texture's class
; from every tex would call this half texture int when the query is the last
; tex to name it -- the order here -- and refuse a kernel whose declaration and
; instructions agree.
source_filename = "tex_size_h"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x half>, i8 } @air.sample_texture_2d.v4f16(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare void @air.write_texture_2d.v4f16(ptr addrspace(1), <2 x i32>, <4 x half>, i32, i32)

define void @tex_size_h(ptr addrspace(1) %in, ptr addrspace(2) %s, ptr addrspace(1) %out, <2 x i32> %gid) local_unnamed_addr {
  %gf = uitofp <2 x i32> %gid to <2 x float>
  %uv = fmul <2 x float> %gf, <float 1.562500e-02, float 1.562500e-02>
  %r = tail call { <4 x half>, i8 } @air.sample_texture_2d.v4f16(ptr addrspace(1) %in, ptr addrspace(2) %s, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x half>, i8 } %r, 0
  %w = tail call i32 @air.get_width_texture_2d(ptr addrspace(1) %in, i32 0)
  %wf = uitofp i32 %w to half
  %w0 = insertelement <4 x half> undef, half %wf, i32 0
  %ws = shufflevector <4 x half> %w0, <4 x half> undef, <4 x i32> zeroinitializer
  %v = fmul <4 x half> %c, %ws
  tail call void @air.write_texture_2d.v4f16(ptr addrspace(1) %out, <2 x i32> %gid, <4 x half> %v, i32 0, i32 0)
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_size_h, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<half, sample>", !"air.arg_name", !"in"}
!6 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!7 = !{i32 2, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<half, write>", !"air.arg_name", !"out"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
