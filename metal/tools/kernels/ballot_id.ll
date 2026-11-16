; ballot_id: a SIMD-group ballot, the LLVM IR a Metal compiler emits for
;
;     kernel void ballot_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       ulong m = (ulong)simd_ballot(gid % 3 == 0);
;       out[gid] = (gid & 1) ? uint(m >> 32) : uint(m);
;     }
;
; Dispatched in groups of 64, one group of 64 lanes each (hakios-metal keeps ir3 to its single threadsize), so the
; mask's bit l is (group base + l) % 3 == 0: a pattern that differs from group to group, and a high half that is not
; zero -- a 32-lane mask or a truncated one reads 0 in every odd word. Resident Evil 4's culling passes elect a lane
; with a ballot, which the front end refused. Hand-written, ours, MIT; the metadata's shape is Apple's convention, as
; in fill_id.ll.

source_filename = "ballot_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare i64 @air.simd_ballot.i64(i1)

define void @ballot_id(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
  %r = urem i32 %gid, 3
  %c = icmp eq i32 %r, 0
  %m = tail call i64 @air.simd_ballot.i64(i1 %c)
  %hi64 = lshr i64 %m, 32
  %hi = trunc i64 %hi64 to i32
  %lo = trunc i64 %m to i32
  %odd = and i32 %gid, 1
  %isodd = icmp ne i32 %odd, 0
  %v = select i1 %isodd, i32 %hi, i32 %lo
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
!2 = !{ptr @ballot_id, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
