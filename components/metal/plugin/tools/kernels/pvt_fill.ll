; pvt_fill, the AIR side: a kernel with a dynamically indexed array, which ir3
; keeps in private memory (stp/ldp), as the LLVM IR a Metal compiler would emit for
;
;     kernel void pvt_fill(device uint* out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       uint a[128];
;       for (uint i = 0; i < 128; i++) a[i] = 3 * i + (gid >> 7) + 1;
;       out[gid] = a[gid & 127];
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention.
;
; Why it exists: the dispatch refused any kernel that spills (pvtmemSize) until
; it learnt to bind the queue's private memory as a draw does. Each thread's
; array depends on its own gid, so a fiber reading another fiber's array, or
; zeros, or nothing at all, gives a word other than 3 (gid & 127) + (gid >> 7) + 1.
source_filename = "pvt_fill"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @pvt_fill(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
entry:
  %a = alloca [128 x i32], align 4
  %hi = lshr i32 %gid, 7
  %base = add i32 %hi, 1
  br label %fill

fill:
  %i = phi i32 [ 0, %entry ], [ %next, %fill ]
  %t = mul i32 %i, 3
  %v = add i32 %t, %base
  %p = getelementptr inbounds [128 x i32], ptr %a, i32 0, i32 %i
  store i32 %v, ptr %p, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 128
  br i1 %more, label %fill, label %read

read:
  %k = and i32 %gid, 127
  %q = getelementptr inbounds [128 x i32], ptr %a, i32 0, i32 %k
  %w = load i32, ptr %q, align 4
  %o = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %gid
  store i32 %w, ptr addrspace(1) %o, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @pvt_fill, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
