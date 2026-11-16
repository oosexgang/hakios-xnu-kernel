; Two fragment shaders that read their own render target, for `air_run --fbfetch`.
;
; WHY: CoreAnimation's inline SDFs (fixed_frag_* with fc_framebuffer_fetch) keep the
; field in a colour attachment and read it back as `[[color(n)]]`, and with them on
; the glass draws opaque black on Adreno 830 (CA_DISABLE_INLINE_SDFS=1 is what mends it).
; Nothing had ever compared a framebuffer fetch against a number -- softpipe has no
; FBFETCH -- and the Adreno 830 takes a different descriptor for it than the 642L
; did (fd6_gmem.cc, patch_fb_read_gmem, CHIP >= A8XX). These two make it a number:
;
;     fragment float4 fbFetch(float4 c [[color(0)]]) { return c * 2 + float4(1, 2, 3, 4); }
;     fragment half4 fbFetchHalf(half4 c [[color(0)]]) { return c * 2 + half4(1, 2, 3, 4); }
;
; air_run --fbfetch clears the target to (0.25, 0.5, 0.75, 1) and draws several
; times, and each draw doubles what is there and adds (1, 2, 3, 4):
;
;     after draw 1    1.5  3  4.5  6       the fetch read the clear
;     after draw 2    4    8  12   16      it read draw 1, in a batch of its own
;     after draw 4    19   38 57   76      draws 3 and 4 in ONE batch, the second
;                                          reading what the first wrote
;
; and the wrong answers name themselves: 1 2 3 4 is a fetch that returned zero,
; 1.5 3 4.5 6 after draw 2 is a fetch that keeps reading the clear.
;
;   llvm-as -o fbfetch.bc tools/air/fbfetch.ll
;   air_run --fbfetch fbfetch.bc fbFetch
;   air_run --fbfetch --half-target fbfetch.bc fbFetchHalf   (the _lph shaders' RGBA16F)
;
; (c + c rather than c * 2: a newer llvm-as writes a splat as a vector-typed float
; constant, which the reader does not take and Apple's compiler never writes)
;
; FD_MESA_DEBUG=gmem or =sysmem in the environment picks freedreno's path, and the
; two descriptors are different code (patch_fb_read_gmem / patch_fb_read_sysmem).

source_filename = "fbFetch"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin26.0.0"

define <{ <4 x float> }> @fbFetch(<4 x float> %0) local_unnamed_addr {
  %2 = fadd <4 x float> %0, %0
  %3 = fadd <4 x float> %2, <float 1.0, float 2.0, float 3.0, float 4.0>
  %4 = insertvalue <{ <4 x float> }> undef, <4 x float> %3, 0
  ret <{ <4 x float> }> %4
}

define <{ <4 x half> }> @fbFetchHalf(<4 x half> %0) local_unnamed_addr {
  %2 = fadd <4 x half> %0, %0
  %3 = fadd <4 x half> %2, <half 1.0, half 2.0, half 3.0, half 4.0>
  %4 = insertvalue <{ <4 x half> }> undef, <4 x half> %3, 0
  ret <{ <4 x half> }> %4
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.compile_options = !{!2}
!air.fragment = !{!3, !8}

!0 = !{i32 2, i32 8, i32 0}
!1 = !{!"Metal", i32 3, i32 2, i32 0}
!2 = !{!"air.compile.framebuffer_fetch_enable"}
!3 = !{ptr @fbFetch, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
!8 = !{ptr @fbFetchHalf, !9, !11}
!9 = !{!10}
!10 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4", !"air.arg_name", !"out"}
!11 = !{!12}
!12 = !{i32 0, !"air.render_target", i32 0, !"air.arg_type_name", !"half4", !"air.arg_name", !"c"}
