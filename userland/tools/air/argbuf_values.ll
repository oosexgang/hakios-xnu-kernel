; Argument buffers with VALUES in them, the shape Darwin 27's VFX sets have: an
; `air.indirect_constant` read where it lies in the set, an array of them indexed at run
; time, an inline struct (its members flattened into the set, numbered from its own
; [[id(n)]]). tools/air/argbuf_atomic.ll is the device half. Dispatched by
; tools/air/argbufvalcheck.c.
;
;     struct Pair { float x [[id(0)]]; float y [[id(1)]]; };
;     struct Vals {                        // constant, 64 bytes
;         float  a      [[id(0)]];         // byte 0
;         uint   b      [[id(1)]];         // byte 4
;         float2 c      [[id(2)]];         // byte 8
;         float4 arr[2] [[id(3)]];         // byte 16
;         Pair   s      [[id(5)]];         // byte 48
;     };
;     kernel void av(constant Vals &v [[buffer(0)]], device float *out [[buffer(1)]],
;                    uint gid [[thread_position_in_grid]]) {
;         out[gid] = v.a + v.b + v.c.y + v.arr[gid & 1].z + v.s.y;
;     }
;
source_filename = "av"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%struct.Pair = type { float, float }
%struct.Vals = type { float, i32, <2 x float>, [2 x <4 x float>], %struct.Pair, [8 x i8] }
define void @av(ptr addrspace(2) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %pa = getelementptr inbounds %struct.Vals, ptr addrspace(2) %0, i64 0, i32 0
  %a = load float, ptr addrspace(2) %pa, align 4
  %pb = getelementptr inbounds %struct.Vals, ptr addrspace(2) %0, i64 0, i32 1
  %b = load i32, ptr addrspace(2) %pb, align 4
  %bf = uitofp i32 %b to float
  %pc = getelementptr inbounds %struct.Vals, ptr addrspace(2) %0, i64 0, i32 2
  %c = load <2 x float>, ptr addrspace(2) %pc, align 8
  %cy = extractelement <2 x float> %c, i64 1
  %i = and i32 %2, 1
  %iw = zext i32 %i to i64
  %pr = getelementptr inbounds %struct.Vals, ptr addrspace(2) %0, i64 0, i32 3, i64 %iw
  %r = load <4 x float>, ptr addrspace(2) %pr, align 16
  %rz = extractelement <4 x float> %r, i64 2
  %ps = getelementptr inbounds %struct.Vals, ptr addrspace(2) %0, i64 0, i32 4, i32 1
  %sy = load float, ptr addrspace(2) %ps, align 4
  %s1 = fadd float %a, %bf
  %s2 = fadd float %s1, %cy
  %s3 = fadd float %s2, %rz
  %s4 = fadd float %s3, %sy
  %w = zext i32 %2 to i64
  %po = getelementptr inbounds float, ptr addrspace(1) %1, i64 %w
  store float %s4, ptr addrspace(1) %po, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @av, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !20, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Vals", !"air.arg_name", !"v"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!20 = !{i32 0, i32 4, i32 0, !"float", !"a", !"air.indirect_argument", !21, i32 4, i32 4, i32 0, !"uint", !"b", !"air.indirect_argument", !22, i32 8, i32 8, i32 0, !"float2", !"c", !"air.indirect_argument", !23, i32 16, i32 32, i32 0, !"float4", !"arr", !"air.indirect_argument", !24, !"air.struct_type_info", !25, i32 48, i32 8, i32 0, !"Pair", !"s", !"air.indirect_argument", i32 5}
!21 = !{i32 0, !"air.indirect_constant", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"a"}
!22 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"b"}
!23 = !{i32 2, !"air.indirect_constant", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"c"}
!24 = !{i32 3, !"air.indirect_constant", !"air.location_index", i32 3, i32 2, !"air.arg_type_name", !"float4", !"air.arg_name", !"arr"}
!25 = !{i32 0, i32 4, i32 0, !"float", !"x", !"air.indirect_argument", !26, i32 4, i32 4, i32 0, !"float", !"y", !"air.indirect_argument", !27}
!26 = !{i32 0, !"air.indirect_constant", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"x"}
!27 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"y"}
