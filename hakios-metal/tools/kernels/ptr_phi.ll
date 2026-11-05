; ptr_phi, the AIR side: pointer phis into one buffer at offsets known only at run
; time, as the LLVM IR a Metal compiler would emit for
;
;     kernel void ptr_phi(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
;                         uint gid [[thread_position_in_grid]])
;     {
;       device const uint* row = (gid & 1) ? in + gid : in + gid + 64;   // a branch, then a phi
;       out[2 * gid] = *row;
;       uint s = 0;
;       device const uint* p = in + gid;                                // stepped round a loop
;       for (uint i = 0; i < 4; i++, p++) s += *p;
;       out[2 * gid + 1] = s;
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention.
;
; Why it exists: the front end resolves a pointer phi while translating, and
; refused one whose operands it could not prove to be the same place -- two
; offsets into one buffer, or an operand over a loop's back edge. Both now carry
; the offset in a register. With in[w] = 3w + 7, out[2g] is in[g] for odd g and
; in[g + 64] for even g, and out[2g + 1] the sum of in[g .. g + 3]: an offset
; register stuck at its first value, or the wrong edge's offset, moves them.
source_filename = "ptr_phi"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @ptr_phi(ptr addrspace(1) %in, ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
entry:
  %odd = and i32 %gid, 1
  %isodd = icmp ne i32 %odd, 0
  br i1 %isodd, label %a, label %b

a:
  %pa = getelementptr inbounds i32, ptr addrspace(1) %in, i32 %gid
  br label %join

b:
  %g64 = add i32 %gid, 64
  %pb = getelementptr inbounds i32, ptr addrspace(1) %in, i32 %g64
  br label %join

join:
  %row = phi ptr addrspace(1) [ %pa, %a ], [ %pb, %b ]
  %rv = load i32, ptr addrspace(1) %row, align 4
  %o0 = shl i32 %gid, 1
  %q0 = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %o0
  store i32 %rv, ptr addrspace(1) %q0, align 4
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %in, i32 %gid
  br label %loop

loop:
  %p = phi ptr addrspace(1) [ %p0, %join ], [ %pn, %loop ]
  %i = phi i32 [ 0, %join ], [ %in1, %loop ]
  %s = phi i32 [ 0, %join ], [ %sn, %loop ]
  %w = load i32, ptr addrspace(1) %p, align 4
  %sn = add i32 %s, %w
  %pn = getelementptr inbounds i32, ptr addrspace(1) %p, i32 1
  %in1 = add i32 %i, 1
  %more = icmp ult i32 %in1, 4
  br i1 %more, label %loop, label %done

done:
  %o1 = add i32 %o0, 1
  %q1 = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %o1
  store i32 %sn, ptr addrspace(1) %q1, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @ptr_phi, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"in"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
