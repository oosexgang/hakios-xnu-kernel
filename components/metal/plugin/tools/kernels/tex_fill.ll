; tex_fill, the AIR side: a kernel that only WRITES a texture, written as the
; LLVM IR a Metal compiler would emit for
;
;     kernel void tex_fill(texture2d<uint, access::write> out [[texture(0)]],
;                          constant uint &c                    [[buffer(0)]],
;                          uint2 gid                           [[thread_position_in_grid]],
;                          uint2 grid                          [[threads_per_grid]])
;     {
;       uint i = gid.y * grid.x + gid.x;
;       out.write(uint4(i * 0x9E3779B1u + c, gid.x, gid.y, 0xdead), gid);
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata and of the intrinsic signature is Apple's convention.
;
; Why a write-only kernel comes first: it adds exactly one new mechanism to a
; dispatch -- a 2D storage descriptor at an IBO slot, written by stib -- and
; none of the others: no texture table, no sampler, no isam coordinate mode.
; The image is [[texture(0)]] of a kernel with no device buffer, so it lands at
; IBO slot num_ssbos + 0 = 0 and no slot before it is left unaddressed; c is a
; pushed constant (a `constant T&`), so no UBO descriptor is needed either.
;
; The first component is fill_id's word for pixel i (an odd multiplier makes
; i -> i*K a bijection, so a readback tells WHICH invocation wrote each pixel),
; the next two are the pixel's own coordinates and the last a constant, so that
; a four-channel format (RGBA32Uint) checks every lane and a one-channel format
; (R32Uint) checks that only .x lands and the other lanes go nowhere.
;
;   llvm-as tex_fill.ll -o tex_fill.bc
;   air2nir tex_fill.bc
;   ir3_compiler --chip-id 0x06030500 tex_fill.bc

source_filename = "tex_fill"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare void @air.write_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, <4 x i32>, i32, i32)

define void @tex_fill(ptr addrspace(1) %out, ptr addrspace(2) %c, <2 x i32> %gid, <2 x i32> %grid) local_unnamed_addr {
  %cv = load i32, ptr addrspace(2) %c, align 4
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %nx = extractelement <2 x i32> %grid, i32 0
  %row = mul i32 %gy, %nx
  %i = add i32 %row, %gx
  %m = mul i32 %i, -1640531535
  %v = add i32 %m, %cv
  %v0 = insertelement <4 x i32> undef, i32 %v, i32 0
  %v1 = insertelement <4 x i32> %v0, i32 %gx, i32 1
  %v2 = insertelement <4 x i32> %v1, i32 %gy, i32 2
  %v3 = insertelement <4 x i32> %v2, i32 57005, i32 3
  tail call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) %out, <2 x i32> %gid, <4 x i32> %v3, i32 0, i32 0)
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_fill, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<uint, write>", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!8 = !{i32 3, !"air.threads_per_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"grid"}
