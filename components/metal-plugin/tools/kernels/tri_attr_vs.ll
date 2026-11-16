; tri_attr_vs, the AIR side: a vertex function whose position and colour come from [[stage_in]] attributes, written as
; the LLVM IR a Metal compiler would emit for
;
;     struct In  { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
;     struct Out { float4 position [[position]]; float4 color; };
;     vertex Out tri_attr_vs(In in [[stage_in]])
;     {
;       return Out{ float4(in.position, 0, 1), in.color };
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention (air.vertex_input nodes with
; air.location_index, as the image's own vertex functions carry them; the same two outputs as tri_color_vs).
;
; Why it exists: it is the first vertex fetch. What the attributes are in memory is not in the shader but in the
; pipeline's vertex descriptor: here one interleaved buffer of 12-byte vertices, the position as Float2 at offset 0
; and the colour as UChar4Normalized at offset 8 -- two formats, two offsets, one stride -- decoded by VFD_DECODE_INSTR
; into the registers VFD_DEST_CNTL_INSTR names. With the colour varying it links to tri_color_fs, so the pixels tell
; whether both attributes arrived: positions wrong move the triangles, colours wrong change the bytes.
;
;   llvm-as tri_attr_vs.ll -o tri_attr_vs.bc
;   air2nir --interface tri_attr_vs.bc
;   ir3_compiler --chip-id 0x06030500 tri_attr_vs.bc

source_filename = "tri_attr_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @tri_attr_vs(<2 x float> %pos, <4 x float> %color) local_unnamed_addr {
  %x = extractelement <2 x float> %pos, i32 0
  %y = extractelement <2 x float> %pos, i32 1
  %v0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %x, i32 0
  %v1 = insertelement <4 x float> %v0, float %y, i32 1
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %v1, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %color, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_attr_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"position"}
!8 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
