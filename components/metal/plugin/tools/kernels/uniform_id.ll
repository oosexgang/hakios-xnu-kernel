; uniform_id: simd_is_uniform over a value every lane of a SIMD group shares, and over one they do not:
;
;     kernel void uniform_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       out[gid] = 2 * uint(simd_is_uniform(ushort(gid >> 8))) + uint(simd_is_uniform(gid & 1));
;     }
;
; Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in fill_id.ll. gid >> 8 is one value over
; 256 consecutive threads, which hold a whole group of this hardware's 64 or 128 lanes; gid & 1 differs between
; neighbours. Every word is 2. air.is_uniform is what mediaserverd's CNN kernels call, which the front end refused.

source_filename = "uniform_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare i1 @air.is_uniform.i16(i16)
declare i1 @air.is_uniform.i32(i32)

define void @uniform_id(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
  %hi = lshr i32 %gid, 8
  %h16 = trunc i32 %hi to i16
  %u1 = call i1 @air.is_uniform.i16(i16 %h16)
  %lo = and i32 %gid, 1
  %u2 = call i1 @air.is_uniform.i32(i32 %lo)
  %z1 = zext i1 %u1 to i32
  %z2 = zext i1 %u2 to i32
  %d = shl i32 %z1, 1
  %v = add i32 %d, %z2
  %t = zext i32 %gid to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %v, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @uniform_id, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
