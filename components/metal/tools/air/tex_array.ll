; Two textures in a local array, read in a loop at the index the loop counts -- CMImaging's srlApply
; and StyleEngine kernels walk a `metal::array<texture2d<half>, N>` so. Thread t reads texel (t, 0) of
; each and writes them to out[2t] and out[2t + 1]; texarraycheck.c fills texture 0 with 1 + x and
; texture 1 with 100 + x.
;
;     kernel void ta(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]],
;                    device float *out [[buffer(0)]], uint t [[thread_position_in_grid]]) {
;         array<texture2d<float>, 2> tex = { a, b };
;         for (uint i = 0; i < 2; i++) out[2 * t + i] = tex[i].read(uint2(t, 0)).x;
;     }
source_filename = "ta"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)

define void @ta(ptr addrspace(1) %a, ptr addrspace(1) %b, ptr addrspace(1) %out, i32 %t) local_unnamed_addr {
entry:
  %tex = alloca [2 x ptr addrspace(1)], align 8
  store ptr addrspace(1) %a, ptr %tex, align 8
  %s1 = getelementptr inbounds [2 x ptr addrspace(1)], ptr %tex, i64 0, i64 1
  store ptr addrspace(1) %b, ptr %s1, align 8
  %c0 = insertelement <2 x i32> zeroinitializer, i32 %t, i64 0
  %rs = tail call ptr addrspace(2) @air.get_read_sampler()
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %iw = zext i32 %i to i64
  %slot = getelementptr inbounds [2 x ptr addrspace(1)], ptr %tex, i64 0, i64 %iw
  %h = load ptr addrspace(1), ptr %slot, align 8
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %h, ptr addrspace(2) %rs, <2 x i32> %c0, <2 x i32> zeroinitializer, i32 0, i32 3)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %x = extractelement <4 x float> %v, i64 0
  %t2 = shl i32 %t, 1
  %o = add i32 %t2, %i
  %ow = zext i32 %o to i64
  %p = getelementptr inbounds float, ptr addrspace(1) %out, i64 %ow
  store float %x, ptr addrspace(1) %p, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 2
  br i1 %more, label %loop, label %done
done:
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ta, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8, !9}
!6 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"a"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"b"}
!8 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!9 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
