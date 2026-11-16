; many_ubo_id: a kernel reading one word from each of ten constant buffers, the LLVM IR a Metal compiler emits for
;
;     kernel void many_ubo_id(device uint *out [[buffer(0)]], constant uint &c0 [[buffer(1)]], ...,
;                             constant uint &c9 [[buffer(10)]], uint gid [[thread_position_in_grid]])
;     {
;       out[gid] = gid + c0 * 1 + c1 * 2 + ... + c9 * 10;
;     }
;
; ir3 pushes each constant buffer it reads at a known offset as a range of its own, so this kernel has ten: more than
; the eight a Kernel held, which refused Resident Evil 4's tonemap (11) and deferred lighting (9) after ir3 had
; compiled them. Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in fill_id.ll.

source_filename = "many_ubo_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @many_ubo_id(ptr addrspace(1) %out, ptr addrspace(2) %c0, ptr addrspace(2) %c1, ptr addrspace(2) %c2, ptr addrspace(2) %c3, ptr addrspace(2) %c4, ptr addrspace(2) %c5, ptr addrspace(2) %c6, ptr addrspace(2) %c7, ptr addrspace(2) %c8, ptr addrspace(2) %c9, i32 %gid) local_unnamed_addr {
  %v0 = load i32, ptr addrspace(2) %c0, align 4
  %m0 = mul i32 %v0, 1
  %s0 = add i32 %gid, %m0
  %v1 = load i32, ptr addrspace(2) %c1, align 4
  %m1 = mul i32 %v1, 2
  %s1 = add i32 %s0, %m1
  %v2 = load i32, ptr addrspace(2) %c2, align 4
  %m2 = mul i32 %v2, 3
  %s2 = add i32 %s1, %m2
  %v3 = load i32, ptr addrspace(2) %c3, align 4
  %m3 = mul i32 %v3, 4
  %s3 = add i32 %s2, %m3
  %v4 = load i32, ptr addrspace(2) %c4, align 4
  %m4 = mul i32 %v4, 5
  %s4 = add i32 %s3, %m4
  %v5 = load i32, ptr addrspace(2) %c5, align 4
  %m5 = mul i32 %v5, 6
  %s5 = add i32 %s4, %m5
  %v6 = load i32, ptr addrspace(2) %c6, align 4
  %m6 = mul i32 %v6, 7
  %s6 = add i32 %s5, %m6
  %v7 = load i32, ptr addrspace(2) %c7, align 4
  %m7 = mul i32 %v7, 8
  %s7 = add i32 %s6, %m7
  %v8 = load i32, ptr addrspace(2) %c8, align 4
  %m8 = mul i32 %v8, 9
  %s8 = add i32 %s7, %m8
  %v9 = load i32, ptr addrspace(2) %c9, align 4
  %m9 = mul i32 %v9, 10
  %s9 = add i32 %s8, %m9
  %t = zext i32 %gid to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %s9, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @many_ubo_id, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9, !10, !11, !12, !13, !14, !15, !16}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c0"}
!7 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c1"}
!8 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c2"}
!9 = !{i32 4, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 4, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c3"}
!10 = !{i32 5, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 5, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c4"}
!11 = !{i32 6, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 6, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c5"}
!12 = !{i32 7, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 7, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c6"}
!13 = !{i32 8, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 8, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c7"}
!14 = !{i32 9, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 9, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c8"}
!15 = !{i32 10, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 10, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c9"}
!16 = !{i32 11, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
