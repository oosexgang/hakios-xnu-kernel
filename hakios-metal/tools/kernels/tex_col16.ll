; tex_col16: what a texture's layout costs a blur. Sixteen reads down a column, 8 rows apart, per thread:
;
;     kernel void tex_col16(texture2d<float, access::read> in [[texture(0)]],
;                           device float *out                 [[buffer(0)]],
;                           uint2 gid                         [[thread_position_in_grid]],
;                           uint2 grid                        [[threads_per_grid]])
;     {
;       float s = 0;
;       for (uint k = 0; k < 16; k++) s += in.read(uint2(gid.x, gid.y + 8 * k)).x;
;       out[gid.y * grid.x + gid.x] = s;
;     }
;
; over a texture 128 rows taller than the grid. A blur's taps fall in other rows, which a linear layout puts a pitch
; apart (other cache lines for every tap) and a tiled one beside each other: the probe times it over a linear R32Float
; texture and a tiled Depth32Float one of the same size.
;
; Hand-written, ours, MIT: only the shape of the metadata and of the intrinsic signature is Apple's convention.

source_filename = "tex_col16"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)

define void @tex_col16(ptr addrspace(1) %in, ptr addrspace(1) %out, <2 x i32> %gid, <2 x i32> %grid) local_unnamed_addr {
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %y0 = add i32 %gy, 0
  %c0a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c0 = insertelement <2 x i32> %c0a, i32 %y0, i32 1
  %r0 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c0, i32 0, i32 0)
  %v0 = extractvalue { <4 x float>, i8 } %r0, 0
  %x0 = extractelement <4 x float> %v0, i32 0
  %s0 = fadd float 0.000000e+00, %x0
  %y1 = add i32 %gy, 8
  %c1a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c1 = insertelement <2 x i32> %c1a, i32 %y1, i32 1
  %r1 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c1, i32 0, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %r1, 0
  %x1 = extractelement <4 x float> %v1, i32 0
  %s1 = fadd float %s0, %x1
  %y2 = add i32 %gy, 16
  %c2a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c2 = insertelement <2 x i32> %c2a, i32 %y2, i32 1
  %r2 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c2, i32 0, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %r2, 0
  %x2 = extractelement <4 x float> %v2, i32 0
  %s2 = fadd float %s1, %x2
  %y3 = add i32 %gy, 24
  %c3a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c3 = insertelement <2 x i32> %c3a, i32 %y3, i32 1
  %r3 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c3, i32 0, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %r3, 0
  %x3 = extractelement <4 x float> %v3, i32 0
  %s3 = fadd float %s2, %x3
  %y4 = add i32 %gy, 32
  %c4a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c4 = insertelement <2 x i32> %c4a, i32 %y4, i32 1
  %r4 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c4, i32 0, i32 0)
  %v4 = extractvalue { <4 x float>, i8 } %r4, 0
  %x4 = extractelement <4 x float> %v4, i32 0
  %s4 = fadd float %s3, %x4
  %y5 = add i32 %gy, 40
  %c5a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c5 = insertelement <2 x i32> %c5a, i32 %y5, i32 1
  %r5 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c5, i32 0, i32 0)
  %v5 = extractvalue { <4 x float>, i8 } %r5, 0
  %x5 = extractelement <4 x float> %v5, i32 0
  %s5 = fadd float %s4, %x5
  %y6 = add i32 %gy, 48
  %c6a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c6 = insertelement <2 x i32> %c6a, i32 %y6, i32 1
  %r6 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c6, i32 0, i32 0)
  %v6 = extractvalue { <4 x float>, i8 } %r6, 0
  %x6 = extractelement <4 x float> %v6, i32 0
  %s6 = fadd float %s5, %x6
  %y7 = add i32 %gy, 56
  %c7a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c7 = insertelement <2 x i32> %c7a, i32 %y7, i32 1
  %r7 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c7, i32 0, i32 0)
  %v7 = extractvalue { <4 x float>, i8 } %r7, 0
  %x7 = extractelement <4 x float> %v7, i32 0
  %s7 = fadd float %s6, %x7
  %y8 = add i32 %gy, 64
  %c8a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c8 = insertelement <2 x i32> %c8a, i32 %y8, i32 1
  %r8 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c8, i32 0, i32 0)
  %v8 = extractvalue { <4 x float>, i8 } %r8, 0
  %x8 = extractelement <4 x float> %v8, i32 0
  %s8 = fadd float %s7, %x8
  %y9 = add i32 %gy, 72
  %c9a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c9 = insertelement <2 x i32> %c9a, i32 %y9, i32 1
  %r9 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c9, i32 0, i32 0)
  %v9 = extractvalue { <4 x float>, i8 } %r9, 0
  %x9 = extractelement <4 x float> %v9, i32 0
  %s9 = fadd float %s8, %x9
  %y10 = add i32 %gy, 80
  %c10a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c10 = insertelement <2 x i32> %c10a, i32 %y10, i32 1
  %r10 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c10, i32 0, i32 0)
  %v10 = extractvalue { <4 x float>, i8 } %r10, 0
  %x10 = extractelement <4 x float> %v10, i32 0
  %s10 = fadd float %s9, %x10
  %y11 = add i32 %gy, 88
  %c11a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c11 = insertelement <2 x i32> %c11a, i32 %y11, i32 1
  %r11 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c11, i32 0, i32 0)
  %v11 = extractvalue { <4 x float>, i8 } %r11, 0
  %x11 = extractelement <4 x float> %v11, i32 0
  %s11 = fadd float %s10, %x11
  %y12 = add i32 %gy, 96
  %c12a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c12 = insertelement <2 x i32> %c12a, i32 %y12, i32 1
  %r12 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c12, i32 0, i32 0)
  %v12 = extractvalue { <4 x float>, i8 } %r12, 0
  %x12 = extractelement <4 x float> %v12, i32 0
  %s12 = fadd float %s11, %x12
  %y13 = add i32 %gy, 104
  %c13a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c13 = insertelement <2 x i32> %c13a, i32 %y13, i32 1
  %r13 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c13, i32 0, i32 0)
  %v13 = extractvalue { <4 x float>, i8 } %r13, 0
  %x13 = extractelement <4 x float> %v13, i32 0
  %s13 = fadd float %s12, %x13
  %y14 = add i32 %gy, 112
  %c14a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c14 = insertelement <2 x i32> %c14a, i32 %y14, i32 1
  %r14 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c14, i32 0, i32 0)
  %v14 = extractvalue { <4 x float>, i8 } %r14, 0
  %x14 = extractelement <4 x float> %v14, i32 0
  %s14 = fadd float %s13, %x14
  %y15 = add i32 %gy, 120
  %c15a = insertelement <2 x i32> undef, i32 %gx, i32 0
  %c15 = insertelement <2 x i32> %c15a, i32 %y15, i32 1
  %r15 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %in, <2 x i32> %c15, i32 0, i32 0)
  %v15 = extractvalue { <4 x float>, i8 } %r15, 0
  %x15 = extractelement <4 x float> %v15, i32 0
  %s15 = fadd float %s14, %x15
  %nx = extractelement <2 x i32> %grid, i32 0
  %row = mul i32 %gy, %nx
  %idx = add i32 %row, %gx
  %t = zext i32 %idx to i64
  %po = getelementptr inbounds float, ptr addrspace(1) %out, i64 %t
  store float %s15, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_col16, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"in"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!8 = !{i32 3, !"air.threads_per_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"grid"}
