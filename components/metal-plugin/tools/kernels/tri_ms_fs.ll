; tri_ms_fs, a fragment function that reads one sample of a multisample texture: the LLVM IR a Metal compiler would
; emit for
;
;     fragment float4 tri_ms_fs(float4 pos [[position]], constant uint &which [[buffer(0)]],
;                               texture2d_ms<float> t [[texture(0)]])
;     {
;       return t.read(uint2(pos.xy), which);
;     }
;
; Each pixel copies sample WHICH of its own pixel of a 4x texture: where a triangle's edge split a pixel's samples,
; different WHICH give different colours, which a read that ignored the sample (or read the pixel as one sample of a
; wider texture) does not.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_ms_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.read_texture_2d_ms.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)

define <4 x float> @tri_ms_fs(<4 x float> %pos, ptr addrspace(2) %which, ptr addrspace(1) %t) local_unnamed_addr {
  %xy = shufflevector <4 x float> %pos, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %c = fptoui <2 x float> %xy to <2 x i32>
  %s = load i32, ptr addrspace(2) %which, align 4
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d_ms.v4f32(ptr addrspace(1) %t, <2 x i32> %c, i32 %s, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_ms_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"which"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d_ms<float, read>", !"air.arg_name", !"t"}
