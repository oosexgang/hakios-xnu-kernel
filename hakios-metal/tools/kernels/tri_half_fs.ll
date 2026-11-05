; tri_half_fs, a fragment function that reads halfs out of a constant buffer at
; offsets that are not all multiples of 4: the LLVM IR a Metal compiler would
; emit for
;
;     struct U { half pad[32]; half a, b, c, d; };   // a at byte 64, b 66, c 68, d 70
;     fragment float4 tri_half_fs(constant U& u [[buffer(0)]])
;     {
;       return float4(u.a, u.b, u.d, u.c);
;     }
;
; which is the shape of CoreAnimation's noise_scale, a half at byte 70 of its
; uniforms that fixed_frag multiplies its noise texture by. With a = 0.25,
; b = 0.5, d = 0.75 and c = 1 every pixel is 40 80 bf ff; a load that took the
; other half of its dword reads c for d or b for a, which are other values. A
; buffer with every other half 0.125 (0x3000) says which neighbour was read.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention.

source_filename = "tri_half_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_half_fs(ptr addrspace(2) %u) local_unnamed_addr {
  %pa = getelementptr inbounds i8, ptr addrspace(2) %u, i64 64
  %pb = getelementptr inbounds i8, ptr addrspace(2) %u, i64 66
  %pc = getelementptr inbounds i8, ptr addrspace(2) %u, i64 68
  %pd = getelementptr inbounds i8, ptr addrspace(2) %u, i64 70
  %a = load half, ptr addrspace(2) %pa, align 2
  %b = load half, ptr addrspace(2) %pb, align 2
  %c = load half, ptr addrspace(2) %pc, align 2
  %d = load half, ptr addrspace(2) %pd, align 2
  %fa = fpext half %a to float
  %fb = fpext half %b to float
  %fc = fpext half %c to float
  %fd = fpext half %d to float
  %v0 = insertelement <4 x float> undef, float %fa, i32 0
  %v1 = insertelement <4 x float> %v0, float %fb, i32 1
  %v2 = insertelement <4 x float> %v1, float %fd, i32 2
  %v3 = insertelement <4 x float> %v2, float %fc, i32 3
  ret <4 x float> %v3
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_half_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 72, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 72, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"U", !"air.arg_name", !"u"}
