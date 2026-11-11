; hole_vs: a vertex function that declares an output it never writes, ahead of its position: the LLVM IR a Metal
; compiler would emit for
;
;     struct Vary { float4 color; float2 unused; float4 position [[position]]; };
;     vertex Vary hole_vs(float4 position [[attribute(0)]], float4 color [[attribute(1)]])
;     {
;       Vary v;
;       v.color = color;
;       v.position = position;
;       return v;            // v.unused left undef
;     }
;
; The front end gives every declared output a driver_location before NIR sees which are written, and ir3 places outputs
; by it, so the unwritten one is a hole in ir3's output table: slot 0 and no register. Slot 0 is VARYING_SLOT_POS, and
; Resident Evil 4's VSGUI (its TexCoord1 undef) was refused as writing no position because the lookup found the hole.
;
; Hand-written, ours, MIT; only the shape of the metadata is Apple's convention (tri_pos_vs.ll).

source_filename = "hole_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <2 x float>, <4 x float> }> @hole_vs(<4 x float> %pos, <4 x float> %color) local_unnamed_addr {
  %r0 = insertvalue <{ <4 x float>, <2 x float>, <4 x float> }> undef, <4 x float> %color, 0
  %r1 = insertvalue <{ <4 x float>, <2 x float>, <4 x float> }> %r0, <4 x float> %pos, 2
  ret <{ <4 x float>, <2 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @hole_vs, !3, !7}
!3 = !{!4, !5, !6}
!4 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!5 = !{!"air.vertex_output", !"generated(6unusedDv2_f)", !"air.arg_type_name", !"float2", !"air.arg_name", !"unused"}
!6 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!7 = !{!8, !9}
!8 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!9 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
