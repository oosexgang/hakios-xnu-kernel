; A fragment shader that writes its own fragment position, and nothing else.
;
; WHY it is hand-written and not taken from the corpus: not one of Apple's 271
; fragment shaders reads its position. All seventeen textured ones declare
; `float4 pos [[position]]` and Apple's own metadata marks every one of them
; `air.arg_unused`; the 254 clearFragment** write a constant. So the corpus
; cannot say where clip y = -1 lands, and a measurement needs a shader whose
; result IS its position. This is that shader:
;
;     fragment float4 fragPos(float4 pos [[position]]) { return pos; }
;
; and what it prints at render-target pixel (0, 0) names the convention, because
; the four components are read back by index and not by eye:
;
;     0.5 0.5 0 1    the position's origin is the top left of the attachment:
;                    the pixel first in memory is the one at y = 0.5. This is
;                    Metal's rule (MSL: [[position]] is a window coordinate with
;                    the origin at the top left, pixel centres at half-integers)
;                    and it is what nir_shader_info.fs.origin_upper_left asks for.
;     0.5 3.5 0 1    OpenGL's rule on a 4-high target: y counted from the other
;                    edge. Measured by setting origin_upper_left to false --
;                    the flag is load-bearing, not decoration.
;
; Written here rather than generated because it is one function of one
; instruction; the metadata keys are the AIR interface (docs/air.md section 3)
; and nothing in this file comes out of Apple's libraries.
;
;   llvm-as -o fragpos.bc tools/air/fragpos.ll
;   build/mesa-host/build/src/gallium/drivers/freedreno/air_run fragpos.bc
;
; On the phone it is one job line of its own; docs/metal.md 10.28 and 10.32 say which
; answers mean what.

source_filename = "fragPos"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @fragPos(<4 x float> %0) local_unnamed_addr {
  %2 = insertvalue <{ <4 x float> }> undef, <4 x float> %0, 0
  ret <{ <4 x float> }> %2
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @fragPos, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
