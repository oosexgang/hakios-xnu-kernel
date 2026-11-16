; null_fs, a fragment function that does nothing: the LLVM IR a Metal compiler would emit for
;
;     fragment void null_fs() {}
;
; What a pipeline with no fragment function runs (MTLRenderPipelineDescriptor.fragmentFunction nil, a depth-only pass's
; shape): the stage writes no colour, so only the depth and stencil the rasterizer and the test produce are drawn. The
; driver carries it as bitcode and compiles it like any other stage, so it links against whatever the vertex stage puts out.
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "null_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @null_fs() local_unnamed_addr {
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @null_fs, !3, !3}
!3 = !{}
