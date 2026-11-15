; The device half of tools/air/argbuf_values.ll: an atomic in an argument buffer, which Apple
; writes as a struct of one int whose descriptor is the bare [[id(n)]], and a value beside it.
; Dispatched by tools/air/argbufvalcheck.c.
;
;     struct Cnt { atomic_int n [[id(0)]]; float scale [[id(1)]]; };   // device, 16 bytes
;     kernel void aa(device Cnt &c [[buffer(0)]], device float *out [[buffer(1)]],
;                    uint gid [[thread_position_in_grid]]) {
;         atomic_fetch_add_explicit(&c.n, 1, memory_order_relaxed);
;         out[gid] = c.scale * gid;
;     }
source_filename = "aa"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%struct.Atom = type { i32 }
%struct.Cnt = type { %struct.Atom, float, [8 x i8] }

declare i32 @air.atomic.global.add.s.i32(ptr addrspace(1), i32, i32, i32, i1) local_unnamed_addr

define void @aa(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %pn = getelementptr inbounds %struct.Cnt, ptr addrspace(1) %0, i64 0, i32 0, i32 0
  %old = call i32 @air.atomic.global.add.s.i32(ptr addrspace(1) %pn, i32 1, i32 0, i32 2, i1 true)
  %pk = getelementptr inbounds %struct.Cnt, ptr addrspace(1) %0, i64 0, i32 1
  %k = load float, ptr addrspace(1) %pk, align 4
  %g = uitofp i32 %2 to float
  %v = fmul float %k, %g
  %w = zext i32 %2 to i64
  %po = getelementptr inbounds float, ptr addrspace(1) %1, i64 %w
  store float %v, ptr addrspace(1) %po, align 4
  ret void
}

!air.kernel = !{!40}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!4 = !{}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!40 = !{ptr @aa, !4, !41}
!41 = !{!42, !7, !8}
!42 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !43, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Cnt", !"air.arg_name", !"c"}
!43 = !{!"air.struct_type_info", !44, i32 0, i32 4, i32 0, !"metal::_atomic", !"n", !"air.indirect_argument", i32 0, i32 4, i32 4, i32 0, !"float", !"scale", !"air.indirect_argument", !46}
!44 = !{i32 0, i32 4, i32 0, !"int", !"__s", !"air.indirect_argument", !45}
!45 = !{i32 0, !"air.indirect_constant", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"int", !"air.arg_name", !"__s"}
!46 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"scale"}
