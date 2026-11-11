; pack_id: four bytes of gid as halves in 0..1, packed back with pack_float_to_unorm4x8, the AIR a Metal compiler
; emits for
;
;     kernel void pack_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       half4 c = half4(gid & 255, (gid >> 2) & 255, 255 - (gid & 255), 128) / 255.0h;
;       out[gid] = pack_float_to_unorm4x8(c);
;     }
;
; Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in fill_id.ll. n / 255 in a half is within
; 0.12 of a step of n, so the round to nearest gives n back exactly: every word is the four bytes, component 0 low.
; VideoToolbox's scaler (the live wallpaper's) packs so, which the front end refused.

source_filename = "pack_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare i32 @air.pack.unorm4x8.v4f16(<4 x half>)

define void @pack_id(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
  %a = and i32 %gid, 255
  %s = lshr i32 %gid, 2
  %b = and i32 %s, 255
  %c = sub i32 255, %a
  %fa = uitofp i32 %a to half
  %fb = uitofp i32 %b to half
  %fc = uitofp i32 %c to half
  %v0 = insertelement <4 x half> poison, half %fa, i64 0
  %v1 = insertelement <4 x half> %v0, half %fb, i64 1
  %v2 = insertelement <4 x half> %v1, half %fc, i64 2
  %v3 = insertelement <4 x half> %v2, half 0xH5800, i64 3
  %n = fdiv <4 x half> %v3, <half 0xH5BF8, half 0xH5BF8, half 0xH5BF8, half 0xH5BF8>
  %p = call i32 @air.pack.unorm4x8.v4f16(<4 x half> %n)
  %t = zext i32 %gid to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %p, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @pack_id, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
