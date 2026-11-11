; bytes_id: a word taken apart into its bytes and put back, the AIR a Metal compiler emits for
;
;     kernel void bytes_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       uchar4 b = as_type<uchar4>(gid * 0x9E3779B1u);
;       out[gid] = uint(b.x) + uint(b.y) + uint(b.z) + uint(b.w) + (as_type<uint>(b.wzyx) & 0xff00u);
;     }
;
; Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in fill_id.ll. The bitcast from 1 x 32 bits
; to 4 x 8 (and back) is what mediaserverd's hbf_compute_residual_error_kernel did, which the front end refused --
; and MPS aborted the process on the refusal, over and over.

source_filename = "bytes_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @bytes_id(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
  %m = mul i32 %gid, -1640531535
  %b = bitcast i32 %m to <4 x i8>
  %b0 = extractelement <4 x i8> %b, i64 0
  %b1 = extractelement <4 x i8> %b, i64 1
  %b2 = extractelement <4 x i8> %b, i64 2
  %b3 = extractelement <4 x i8> %b, i64 3
  %z0 = zext i8 %b0 to i32
  %z1 = zext i8 %b1 to i32
  %z2 = zext i8 %b2 to i32
  %z3 = zext i8 %b3 to i32
  %s1 = add i32 %z0, %z1
  %s2 = add i32 %s1, %z2
  %s3 = add i32 %s2, %z3
  %r = shufflevector <4 x i8> %b, <4 x i8> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
  %w = bitcast <4 x i8> %r to i32
  %k = and i32 %w, 65280
  %v = add i32 %s3, %k
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
!2 = !{ptr @bytes_id, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
