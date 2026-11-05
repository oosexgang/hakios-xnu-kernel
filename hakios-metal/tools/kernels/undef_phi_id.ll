; undef_phi_id: a pointer split into one phi per path, each undef over the other path's edge -- the shape LLVM leaves
; when only one path uses each copy:
;
;     kernel void undef_phi_id(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]])
;     {
;       if (gid & 1) out[gid] = gid * 3;
;       else         out[gid] = gid ^ 0x5a5a;
;     }
;
; after the optimiser has hoisted the two address computations above a join and sunk the stores below it. Hand-written,
; ours, MIT; the metadata's shape is Apple's convention, as in fill_id.ll. MPS's cnnConvArray_* kernels carry two such
; threadgroup pointers, and the front end counted the undef as a second resource and refused them, which took
; mediaserverd down in a loop. The two values differ for every gid, so a store through the wrong copy shows.

source_filename = "undef_phi_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @undef_phi_id(ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
entry:
  %t = zext i32 %gid to i64
  %odd = trunc i32 %gid to i1
  br i1 %odd, label %a, label %b

a:
  %pa = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  br label %join

b:
  %pb = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  br label %join

join:
  %p1 = phi ptr addrspace(1) [ %pa, %a ], [ undef, %b ]
  %p2 = phi ptr addrspace(1) [ undef, %a ], [ %pb, %b ]
  br i1 %odd, label %s1, label %s2

s1:
  %v1 = mul i32 %gid, 3
  store i32 %v1, ptr addrspace(1) %p1, align 4
  br label %end

s2:
  %v2 = xor i32 %gid, 23130
  store i32 %v2, ptr addrspace(1) %p2, align 4
  br label %end

end:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @undef_phi_id, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
