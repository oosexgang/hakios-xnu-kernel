; tri_dwrite_fs, tri_dbuf_fs's twin that also WRITES its device buffer -- what a draw replayed tile by tile would do once a
; tile (compileStage's writesMemory, render::gmemDrawOk):
;
;     fragment float4 tri_dwrite_fs(device float4 *c [[buffer(0)]])
;     {
;       float4 v = c[0];
;       c[1] = v;
;       return v;
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple; only the SHAPE of the metadata is Apple's convention.

source_filename = "tri_dwrite_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_dwrite_fs(ptr addrspace(1) %c) local_unnamed_addr {
  %v = load <4 x float>, ptr addrspace(1) %c, align 16
  %p = getelementptr inbounds <4 x float>, ptr addrspace(1) %c, i64 1
  store <4 x float> %v, ptr addrspace(1) %p, align 16
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_dwrite_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
