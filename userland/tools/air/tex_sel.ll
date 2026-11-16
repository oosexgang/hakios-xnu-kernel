; A texture chosen between two members of an argument buffer and read through a 64-bit integer --
; VFX's particle_ribbon_frag selects one of its material's two ribbon textures, loads the member as
; `i64` and samples through `inttoptr` of it. Thread t reads texel (t, 0) of texture b when t is odd
; and of texture a when it is even, and writes it to out[2t + 1] (texarraycheck.c with `sel`:
; 100 + t or 1 + t); out[2t] is left 0.
;
;     struct Set { texture2d<float, access::read> a [[id(0)]]; texture2d<float, access::read> b [[id(1)]]; };
;     kernel void ts(constant Set &set [[buffer(0)]], device float *out [[buffer(1)]],
;                    uint t [[thread_position_in_grid]]) {
;         out[2 * t + 1] = ((t & 1) ? set.b : set.a).read(uint2(t, 0)).x;
;     }
source_filename = "ts"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%"struct.metal::texture2d" = type { ptr addrspace(1) }
%struct.Set = type { %"struct.metal::texture2d", %"struct.metal::texture2d" }

declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)

define void @ts(ptr addrspace(2) %set, ptr addrspace(1) %out, i32 %t) local_unnamed_addr {
  %bit = and i32 %t, 1
  %odd = icmp ne i32 %bit, 0
  %pa = getelementptr inbounds %struct.Set, ptr addrspace(2) %set, i64 0, i32 0
  %pb = getelementptr inbounds %struct.Set, ptr addrspace(2) %set, i64 0, i32 1
  %p = select i1 %odd, ptr addrspace(2) %pb, ptr addrspace(2) %pa
  %h = load i64, ptr addrspace(2) %p, align 8
  %tx = inttoptr i64 %h to ptr addrspace(1)
  %c = insertelement <2 x i32> zeroinitializer, i32 %t, i64 0
  %rs = tail call ptr addrspace(2) @air.get_read_sampler()
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %tx, ptr addrspace(2) %rs, <2 x i32> %c, <2 x i32> zeroinitializer, i32 0, i32 3)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %x = extractelement <4 x float> %v, i64 0
  %t2 = shl i32 %t, 1
  %o = add i32 %t2, 1
  %ow = zext i32 %o to i64
  %po = getelementptr inbounds float, ptr addrspace(1) %out, i64 %ow
  store float %x, ptr addrspace(1) %po, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ts, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !20, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Set", !"air.arg_name", !"set"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
!20 = !{i32 0, i32 8, i32 0, !"texture2d<float, read>", !"a", !"air.indirect_argument", !21, i32 8, i32 8, i32 0, !"texture2d<float, read>", !"b", !"air.indirect_argument", !22}
!21 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"a"}
!22 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"b"}
