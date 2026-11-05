; tri_cflat_fs, the AIR side: tri_flat_fs with its colour read from a constant buffer instead of a literal, written as
; the LLVM IR a Metal compiler would emit for
;
;     fragment float4 tri_cflat_fs(constant float4 &c [[buffer(0)]])
;     {
;       return c;
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention (the air.fragment node, the
; air.render_target output, an air.buffer input of address space 2 with air.buffer_size).
;
; Why it exists: it is the first buffer in a draw. A `constant T&` read at a constant offset is a range ir3 pushes
; into the constant file (ubo_state), the same mechanism compute's fill_id_air uses -- so the draw uploads it with
; CP_LOAD_STATE6_FRAG ST6_CONSTANTS SS6_INDIRECT into SB6_FS_SHADER straight out of the bound buffer, and no UBO
; descriptor or IBO table is involved. Two draws with two colours in the buffer must give two colours.
;
;   llvm-as tri_cflat_fs.ll -o tri_cflat_fs.bc
;   air2nir tri_cflat_fs.bc
;   ir3_compiler --chip-id 0x06030500 tri_cflat_fs.bc

source_filename = "tri_cflat_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_cflat_fs(ptr addrspace(2) %c) local_unnamed_addr {
  %v = load <4 x float>, ptr addrspace(2) %c, align 16
  ret <4 x float> %v
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_cflat_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"c"}
