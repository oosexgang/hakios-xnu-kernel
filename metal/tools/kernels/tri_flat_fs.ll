; tri_flat_fs, the AIR side of the first draw's fragment function: the LLVM IR
; a Metal compiler would emit for
;
;     fragment float4 tri_flat_fs()
;     {
;       return float4(32.0 / 255.0, 64.0 / 255.0, 128.0 / 255.0, 1.0);   // [[color(0)]]
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE of
; the metadata is Apple's convention -- the air.fragment node and the
; air.render_target output node (target 0, dual-source index 0) as QuartzCore's
; path_blit_frag_lpf spells them; the input list is empty, as a function with
; no parameters has it.
;
; The colour is a literal and not a `constant float4&`: the first draw binds no
; buffer in either stage (the contract's first decision, hakios-metal 4c8c901),
; so this FS has constlen 0 and pushes nothing. The four values are exactly the
; RGBA8Unorm bytes 0x20, 0x40, 0x80, 0xff after the hardware's round-to-nearest
; (n/255 for integral n is reproduced by UNORM conversion without ambiguity), so
; the probe compares pixels for equality and not within a tolerance. The
; literals are written in LLVM's hex form because 32/255 has no exact decimal
; spelling as a float: 0x3FC0101020000000 is the float nearest 0.12549019607843137
; (bits 0x3e008081), 0x3FD0101020000000 the one nearest 0.25098039215686274
; (0x3e808081), 0x3FE0101020000000 the one nearest 0.5019607843137255 (0x3f008081).
;
;   llvm-as tri_flat_fs.ll -o tri_flat_fs.bc
;   air2nir tri_flat_fs.bc
;   air2nir --interface tri_flat_fs.bc
;   ir3_compiler --chip-id 0x06030500 tri_flat_fs.bc

source_filename = "tri_flat_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_flat_fs() local_unnamed_addr {
  ret <4 x float> <float 0x3FC0101020000000, float 0x3FD0101020000000, float 0x3FE0101020000000, float 1.000000e+00>
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_flat_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{}
