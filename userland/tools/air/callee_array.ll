; A callee that returns an array of numbers, as MPS's matrix products hand back their
; `[4 x float]` accumulators, checked by tools/air/wordcheck.sh: 1, 2, 3, 4 and their sum.
;
;     static array<float, 4> acc(float x) { return { x, 2 * x, 3 * x, 4 * x }; }
;     kernel void ca(device float *out [[buffer(0)]]) {
;         auto a = acc(1); out[0..3] = a[0..3]; out[4] = a[0] + a[1] + a[2] + a[3]; }
source_filename = "ca"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define internal [4 x float] @acc(float %x) {
  %x2 = fmul float %x, 2.0
  %x3 = fmul float %x, 3.0
  %x4 = fmul float %x, 4.0
  %a0 = insertvalue [4 x float] undef, float %x, 0
  %a1 = insertvalue [4 x float] %a0, float %x2, 1
  %a2 = insertvalue [4 x float] %a1, float %x3, 2
  %a3 = insertvalue [4 x float] %a2, float %x4, 3
  ret [4 x float] %a3
}

define void @ca(ptr addrspace(1) %0) local_unnamed_addr {
  %a = call [4 x float] @acc(float 1.0)
  %e0 = extractvalue [4 x float] %a, 0
  %e1 = extractvalue [4 x float] %a, 1
  %e2 = extractvalue [4 x float] %a, 2
  %e3 = extractvalue [4 x float] %a, 3
  store float %e0, ptr addrspace(1) %0, align 4
  %p1 = getelementptr inbounds float, ptr addrspace(1) %0, i64 1
  store float %e1, ptr addrspace(1) %p1, align 4
  %p2 = getelementptr inbounds float, ptr addrspace(1) %0, i64 2
  store float %e2, ptr addrspace(1) %p2, align 4
  %p3 = getelementptr inbounds float, ptr addrspace(1) %0, i64 3
  store float %e3, ptr addrspace(1) %p3, align 4
  %s1 = fadd float %e0, %e1
  %s2 = fadd float %s1, %e2
  %s3 = fadd float %s2, %e3
  %p4 = getelementptr inbounds float, ptr addrspace(1) %0, i64 4
  store float %s3, ptr addrspace(1) %p4, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ca, !4, !5}
!4 = !{}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
