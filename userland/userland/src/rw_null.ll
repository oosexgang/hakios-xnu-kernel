; src/rw_null.metal as AIR, written by hand so that a build without Apple's Metal compiler has it too:
;
;     fragment half4 rw_null_fragment() { discard_fragment(); return half4(0); }
;
; HakiosMetal's stand-in for CoreAnimation's generic uber shader (rw_generic_mode in metal_driver.m): it
; draws nothing. Assembled with llvm-as and turned into rw_null_air.h at build time (rw_null_air[] and
; rw_null_air_len, the bare module as libhakios-air takes one).

source_filename = "rw_null"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

declare void @air.discard_fragment() local_unnamed_addr

define <{ <4 x half> }> @rw_null_fragment() local_unnamed_addr {
  call void @air.discard_fragment()
  ret <{ <4 x half> }> zeroinitializer
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @rw_null_fragment, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4", !"air.arg_name", !"out"}
!5 = !{}
