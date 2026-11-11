; tri_flat_vs, the AIR side of the first draw's vertex function: the LLVM IR a
; Metal compiler would emit for
;
;     vertex float4 tri_flat_vs(uint vid [[vertex_id]])
;     {
;       switch (vid) {                 // two triangles, six vertices, no buffer
;         case 0:  return float4(-1, -1, 0, 1);
;         case 1:  return float4( 1, -1, 0, 1);
;         case 2:  return float4(-1,  1, 0, 1);
;         case 3:  return float4( 1, -1, 0, 1);
;         case 4:  return float4( 1,  1, 0, 1);
;         default: return float4(-1,  1, 0, 1);   // vertex 5, and every vid past it
;       }
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE of
; the metadata is Apple's convention -- the air.vertex node (function, outputs,
; inputs), the air.position output node and the air.vertex_id input node as
; QuartzCore's path_blit_vert_lpf spells them -- which is what the AIR front end
; reads; the values are this shader's own.
;
; Why the positions are literals chosen by compare-and-select and not read from
; a buffer: the first draw binds NO buffer in either stage (the contract's first
; decision, hakios-metal 4c8c901). No Mesa 25.0.7 driver drives the graphics
; IBO table non-bindless, so a `device float2 *p` indexed by vid would need a
; load state no witness emits; turnip's own clear path draws from exactly this
; shape of VS (nir_load_vertex_id and bcsel of load_const, tu_clear_blit.cc:
; 629-664). The chain is scalar icmp eq against each vid and a select of whole
; float4 literals, which the front end makes bcsel of (air_to_nir.c, the
; AIR_OP_SELECT case: a 1-component condition is broadcast over the vector).
;
; Under Metal's orientation (NDC y up, row 0 at the top) vertices 0,1,2 are the
; LOWER-LEFT half of the target (T1) and 3,4,5 its complement (T2): a draw of
; 3 vertices paints T1, a draw of 6 paints both and tests the shared diagonal.
;
;   llvm-as tri_flat_vs.ll -o tri_flat_vs.bc
;   air2nir tri_flat_vs.bc                               (the NIR)
;   air2nir --interface tri_flat_vs.bc                   (what the pipeline must serve)
;   ir3_compiler --chip-id 0x06030500 tri_flat_vs.bc     (the Adreno listing)

source_filename = "tri_flat_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_flat_vs(i32 %vid) local_unnamed_addr {
  ; the chain runs from vertex 4 down to vertex 0 so that the default (vertex 5's
  ; position) is also what every vid of 6 or more gets
  %c4 = icmp eq i32 %vid, 4
  %p4 = select i1 %c4, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %c3 = icmp eq i32 %vid, 3
  %p3 = select i1 %c3, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p4
  %c2 = icmp eq i32 %vid, 2
  %p2 = select i1 %c2, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p3
  %c1 = icmp eq i32 %vid, 1
  %p1 = select i1 %c1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %c0 = icmp eq i32 %vid, 0
  %p0 = select i1 %c0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  ret <4 x float> %p0
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_flat_vs, !3, !5}
!3 = !{!4}
!4 = !{!"air.position", !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
