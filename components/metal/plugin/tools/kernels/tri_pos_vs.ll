; tri_pos_vs, the vertex function the depth tests draw with: the LLVM IR a Metal
; compiler would emit for
;
;     struct In   { float4 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
;     struct Vary { float4 position [[position]]; float4 color; };
;     vertex Vary tri_pos_vs(In in [[stage_in]])
;     {
;       return Vary{in.position, in.color};
;     }
;
; tri_attr_vs with the whole clip-space position from the vertex buffer instead
; of its x and y: a depth test needs triangles at chosen depths (z / w after the
; viewport's [0, 1] mapping), and the colour says which one a pixel kept. Paired
; with tri_color_fs (the colour passed through).
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the
; metadata is Apple's convention (tri_attr_vs.ll).
;
;   llvm-as tri_pos_vs.ll -o tri_pos_vs.bc
;   ir3_compiler --chip-id 0x06030500 tri_pos_vs.bc

source_filename = "tri_pos_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @tri_pos_vs(<4 x float> %pos, <4 x float> %color) local_unnamed_addr {
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %pos, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %color, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_pos_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!8 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
