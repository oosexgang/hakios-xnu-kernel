; Metal's RGB9E5 shared-exponent unpack and pack, folded by air2nir --eval (tools/air/rgb9e5check.sh):
; the word 0x80050100 -- mantissas 256, 128 and 1, exponent 16 -- is (1, 0.5, 1/256), and packing
; those three back gives the same word, read out as the bits of the fourth lane (-4.59536213e-40).
; MTLBVHBuilder's and ComputeGraph's copy kernels read RGB9E5 textures through these.
source_filename = "e9"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"
declare <3 x float> @air.unpack.unorm.rgb9e5.v3f32(i32)
declare i32 @air.pack.unorm.rgb9e5.v3f32(<3 x float>)
define <{ <4 x float> }> @e9() local_unnamed_addr {
  %v = tail call <3 x float> @air.unpack.unorm.rgb9e5.v3f32(i32 2147811584)
  %p = tail call i32 @air.pack.unorm.rgb9e5.v3f32(<3 x float> <float 1.0, float 0.5, float 0.00390625>)
  %pf = bitcast i32 %p to float
  %a = shufflevector <3 x float> %v, <3 x float> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 0>
  %b = insertelement <4 x float> %a, float %pf, i64 3
  %o = insertvalue <{ <4 x float> }> undef, <4 x float> %b, 0
  ret <{ <4 x float> }> %o
}
!air.fragment = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @e9, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{}
