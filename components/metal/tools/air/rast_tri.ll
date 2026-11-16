; Two triangles for tools/air/rastcheck.c, their corners in a constant buffer: the left one goes
; round clockwise as it is seen (bottom left, top, bottom right) and the right one counter-
; clockwise, so which of them a cull mode and a winding leave says which way round each is taken.
;
;     vertex float4 rv(constant float4 *corner [[buffer(0)]], uint v [[vertex_id]]) { return corner[v]; }
;     fragment float4 rf() { return float4(1, 0, 0, 1); }
source_filename = "rast"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define <{ <4 x float> }> @rv(ptr addrspace(2) %0, i32 %1) local_unnamed_addr {
  %w = zext i32 %1 to i64
  %p = getelementptr inbounds <4 x float>, ptr addrspace(2) %0, i64 %w
  %c = load <4 x float>, ptr addrspace(2) %p, align 16
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %c, 0
  ret <{ <4 x float> }> %r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.vertex = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @rv, !4, !6}
!4 = !{!5}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 96, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"corner"}
!8 = !{i32 1, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"v"}
