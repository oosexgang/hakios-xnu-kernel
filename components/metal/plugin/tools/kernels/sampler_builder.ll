; A kernel that builds a `coreimage::Sampler` with a function call and samples through its members: the shape of
; the kernels Apple's airstitcher makes for CoreImage (iconservicesagent's ci_asgDownH_affine_nearest), whose
; `_ci_sampler_builder` returns { texture2d, sampler, constant float4 *transform, float2 } and whose sampling
; function takes the four members after it. The front end refused it -- "a call passing a pointer inside a value"
; -- because the struct came back from the builder with its pointer members empty. Those kernels also start with
; `if (_ci_early_out(gid)) return;`, a call returning an i1 that is then branched on, which came back 32 bits wide
; and stopped nir_lower_goto_ifs; early_out below is that. Hand-written, not Apple's.
;
;     struct Sampler { texture2d<float> t; sampler s; constant float4 *xf; float2 bias; };
;     static Sampler builder(texture2d<float> t, sampler s, constant float4 *xf, float2 bias) { return {t, s, xf, bias}; }
;     static float4 sample_at(texture2d<float> t, sampler s, constant float4 *xf, float2 bias, float2 c)
;         { return t.sample(s, c * xf->xy + xf->zw + bias); }
;     kernel void sampler_builder(constant float4 *args [[buffer(0)]], sampler s [[sampler(0)]],
;                                 texture2d<float> in [[texture(0)]], device uint *out [[buffer(1)]],
;                                 uint2 gid [[thread_position_in_grid]])
;     {
;         if (early_out(gid)) return;                       // gid == (3, 0): its word stays as air_run filled it
;         Sampler k = builder(in, s, args + 2, args[1].xy);
;         float4 v = sample_at(k.t, k.s, k.xf, k.bias, float2(gid));
;         out[gid.y * 4 + gid.x] = uint(v.x) * 1000 + uint(v.y);
;     }
;
; Run on this machine (softpipe): userland/tools/air/sampler_builder.py. The texture is air_run's 4x4 one, texel
; (x, y) = (100 + x, 200 + y, ...), and the sampler air_run binds for a kernel takes coordinates in texels. args[2]
; mirrors x, (-1, 1, 3.5, 0.5), and args[1] adds one to y, so thread (x, y) reads texel (3 - x, min(y + 1, 3)).
; args[0] is a decoy -- the identity, (1, 1, 0.5, 0.5) -- so a transform pointer that lost its offset reads texel
; (x, y + 1) instead, and no thread's answer is the same. Thread (3, 0) returns early, so its word is air_run's
; fill pattern; an early-out taken by every thread, or by none, changes other words.

source_filename = "sampler_builder"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }
%"struct.coreimage::Sampler" = type { %"struct.metal::texture2d", %"struct.metal::sampler", ptr addrspace(2), <2 x float> }

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define internal fastcc %"struct.coreimage::Sampler" @builder(ptr addrspace(1) %t, ptr addrspace(2) %s, ptr addrspace(2) %xf, <2 x float> %bias) {
  %a = insertvalue %"struct.coreimage::Sampler" poison, ptr addrspace(1) %t, 0, 0
  %b = insertvalue %"struct.coreimage::Sampler" %a, ptr addrspace(2) %s, 1, 0
  %c = insertvalue %"struct.coreimage::Sampler" %b, ptr addrspace(2) %xf, 2
  %d = insertvalue %"struct.coreimage::Sampler" %c, <2 x float> %bias, 3
  ret %"struct.coreimage::Sampler" %d
}

define internal fastcc <4 x float> @sample_at(%"struct.metal::texture2d" %t, %"struct.metal::sampler" %s, ptr addrspace(2) %xf, <2 x float> %bias, <2 x float> %coord) {
  %tp = extractvalue %"struct.metal::texture2d" %t, 0
  %sp = extractvalue %"struct.metal::sampler" %s, 0
  %m = load <4 x float>, ptr addrspace(2) %xf, align 16
  %mxy = shufflevector <4 x float> %m, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %mzw = shufflevector <4 x float> %m, <4 x float> poison, <2 x i32> <i32 2, i32 3>
  %p0 = fmul <2 x float> %coord, %mxy
  %p1 = fadd <2 x float> %p0, %mzw
  %p2 = fadd <2 x float> %p1, %bias
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tp, ptr addrspace(2) %sp, <2 x float> %p2, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

define internal fastcc zeroext i1 @early_out(<2 x i32> %gid) {
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %a = icmp eq i32 %x, 3
  %b = icmp eq i32 %y, 0
  %r = and i1 %a, %b
  ret i1 %r
}

define void @sampler_builder(ptr addrspace(2) %args, ptr addrspace(2) %s, ptr addrspace(1) %in, ptr addrspace(1) %out, <2 x i32> %gid) local_unnamed_addr {
entry:
  %skip = call fastcc i1 @early_out(<2 x i32> %gid)
  br i1 %skip, label %done, label %work

work:
  %xfp = getelementptr i8, ptr addrspace(2) %args, i64 32
  %bp = getelementptr i8, ptr addrspace(2) %args, i64 16
  %b4 = load <4 x float>, ptr addrspace(2) %bp, align 16
  %bias = shufflevector <4 x float> %b4, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %k = call fastcc %"struct.coreimage::Sampler" @builder(ptr addrspace(1) %in, ptr addrspace(2) %s, ptr addrspace(2) %xfp, <2 x float> %bias)
  %kt = extractvalue %"struct.coreimage::Sampler" %k, 0
  %ks = extractvalue %"struct.coreimage::Sampler" %k, 1
  %kx = extractvalue %"struct.coreimage::Sampler" %k, 2
  %kb = extractvalue %"struct.coreimage::Sampler" %k, 3
  %gf = uitofp <2 x i32> %gid to <2 x float>
  %v = call fastcc <4 x float> @sample_at(%"struct.metal::texture2d" %kt, %"struct.metal::sampler" %ks, ptr addrspace(2) %kx, <2 x float> %kb, <2 x float> %gf)
  %vx = extractelement <4 x float> %v, i64 0
  %vy = extractelement <4 x float> %v, i64 1
  %ux = fptoui float %vx to i32
  %uy = fptoui float %vy to i32
  %ux1000 = mul i32 %ux, 1000
  %w = add i32 %ux1000, %uy
  %gx = extractelement <2 x i32> %gid, i64 0
  %gy = extractelement <2 x i32> %gid, i64 1
  %row = shl i32 %gy, 2
  %idx = add i32 %row, %gx
  %t = zext i32 %idx to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %w, ptr addrspace(1) %po, align 4
  br label %done

done:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @sampler_builder, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"args"}
!6 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!7 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"in"}
!8 = !{i32 3, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!9 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
