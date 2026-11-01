; tri_color_fs, the AIR side of the per-vertex-colour fragment function: the
; LLVM IR a Metal compiler would emit for
;
;     struct Vary { float4 position [[position]]; float4 color; };
;     fragment float4 tri_color_fs(Vary in [[stage_in]])
;     {
;       return in.color;   // [[color(0)]], interpolated
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE is
; Apple's convention: AIR has no stage-in struct, each member the function reads
; is a parameter of its own carrying an air.fragment_input node with the
; mangled varying name and the interpolation keys (air.center, air.perspective:
; centre-sampled, perspective-correct, which is Metal's default), as QuartzCore's
; upsample_frag_lpf spells them. A member nothing reads (the position here) is
; left out of the parameter list, as Apple's compiler leaves out what the body
; does not use when the member is not [[position]].
;
; What the front end makes of it: a shader_in variable at VARYING_SLOT_VAR0 with
; INTERP_MODE_SMOOTH, which ir3 reads with bary.f through the ij_pixel
; barycentrics (SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL), so the variant carries
; one bary input at inloc 0 (compmask 0xf) and total_in 4. The name the front end
; records for this side is air.arg_name ("color"), not the mangled string; a
; by-name link has to read the latter (air-vs-fs.md, the varying-names gap).
;
;   llvm-as tri_color_fs.ll -o tri_color_fs.bc
;   air2nir tri_color_fs.bc
;   air2nir --interface tri_color_fs.bc
;   ir3_compiler --chip-id 0x06030500 tri_color_fs.bc

source_filename = "tri_color_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_color_fs(<4 x float> %color) local_unnamed_addr {
  ret <4 x float> %color
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_color_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
