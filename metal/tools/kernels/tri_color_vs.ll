; tri_color_vs, the AIR side of the per-vertex-colour vertex function: the LLVM
; IR a Metal compiler would emit for
;
;     struct Vary { float4 position [[position]]; float4 color; };
;     vertex Vary tri_color_vs(uint vid [[vertex_id]])
;     {
;       Vary v;                        // the six positions of tri_flat_vs ...
;       switch (vid) {
;         case 0:  v.position = float4(-1, -1, 0, 1); v.color = float4(1, 0, 0, 1); break;   // red
;         case 1:  v.position = float4( 1, -1, 0, 1); v.color = float4(0, 1, 0, 1); break;   // green
;         case 2:  v.position = float4(-1,  1, 0, 1); v.color = float4(0, 0, 1, 1); break;   // blue
;         case 3:  v.position = float4( 1, -1, 0, 1); v.color = float4(1, 0, 0, 1); break;   // red
;         case 4:  v.position = float4( 1,  1, 0, 1); v.color = float4(0, 1, 0, 1); break;   // green
;         default: v.position = float4(-1,  1, 0, 1); v.color = float4(0, 0, 1, 1); break;   // blue: vertex 5, and every vid past it
;       }
;       return v;                      // ... and a colour varying beside each
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE is
; Apple's convention: the returned struct is spelled packed (<{ }>) and its
; fields carry one output node each, air.position for the first and
; air.vertex_output with the mangled varying name for the second, as
; QuartzCore's std_vert0_lpf spells them. The varying's name
; "generated(5colorDv4_f)" is what a by-name link would match to the fragment
; side; the slot is the order of appearance (VARYING_SLOT_VAR0 here,
; air_to_nir.c translate_output), and with one varying both sides land on VAR0
; without a link step.
;
; No buffer in either stage (the contract's first decision): both the position
; and the colour are literals chosen by icmp eq + select on the vertex id, the
; shape of tri_flat_vs. Red at 0 and 3, green at 1 and 4, blue at 2 and 5, so
; that a pixel's colour is its barycentric weights in the triangle that painted
; it: (bary_A, bary_B, bary_C, 1) for either triangle.
;
;   llvm-as tri_color_vs.ll -o tri_color_vs.bc
;   air2nir tri_color_vs.bc
;   air2nir --interface tri_color_vs.bc
;   ir3_compiler --chip-id 0x06030500 tri_color_vs.bc

source_filename = "tri_color_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @tri_color_vs(i32 %vid) local_unnamed_addr {
  %c4 = icmp eq i32 %vid, 4
  %c3 = icmp eq i32 %vid, 3
  %c2 = icmp eq i32 %vid, 2
  %c1 = icmp eq i32 %vid, 1
  %c0 = icmp eq i32 %vid, 0
  ; the position, from vertex 4 down to vertex 0 over vertex 5's default
  %p4 = select i1 %c4, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %p3 = select i1 %c3, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p4
  %p2 = select i1 %c2, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p3
  %p1 = select i1 %c1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %p0 = select i1 %c0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  ; the colour, the same way: green at 4, red at 3, blue at 2, green at 1, red at 0, blue by default
  %k4 = select i1 %c4, <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>
  %k3 = select i1 %c3, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k4
  %k2 = select i1 %c2, <4 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>, <4 x float> %k3
  %k1 = select i1 %c1, <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k2
  %k0 = select i1 %c0, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %k1
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %k0, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_color_vs, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{!7}
!7 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
