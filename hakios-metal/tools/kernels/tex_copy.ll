; tex_copy, the AIR side: a texture read and a texture write in one compute
; kernel, written as the LLVM IR a Metal compiler would emit for
;
;     kernel void tex_copy(texture2d<uint, access::read>  in   [[texture(1)]],
;                          texture2d<uint, access::write> out  [[texture(0)]],
;                          uint2 gid                           [[thread_position_in_grid]])
;     {
;       out.write(in.read(gid), gid);
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE of the
; metadata and of the two intrinsic signatures is Apple's convention (the
; air.kernel node, the per-argument air.texture nodes, the version nodes, the
; argument order of air.read_texture_2d and air.write_texture_2d as the AIR
; front end reads them, src/compiler/air/air_intrinsics.c "-- textures --");
; the values are this kernel's own.
;
; What the two paths must meet on: texel (x, y) of `out` equals texel (x, y) of
; `in` for every (x, y) the grid covers, and nothing else of `out` changes. The
; input is the caller's: the reference is the input itself (a copy has no
; arithmetic to get wrong), so the test's input must make every texel distinct
; -- (x + 16*y, 0x100 + x, 0x200 + y, 0x300) or any other injective pattern --
; so that a wrong coordinate, a wrong slot or a wrong channel cannot read back
; as the right answer.
;
; The front end turns `in` (air.texture + air.read) into a TEXTURE resource at
; index 1 and `out` (air.texture + air.write) into an IMAGE resource at index 0
; (air_to_nir.c, translate_input's air.texture branch); the read becomes a
; nir_tex_instr txf with texture_index 1 and the write a nir_image_store with
; image index 0 (air_intrinsics.c, texture_op). ir3 then fetches through texture
; slot 1 (isaml t#1, s#0) and stores through IBO slot num_ssbos + 0 = 0 (stib).
; The indices are chosen so that the image lands at IBO slot 0 with no slot
; before it (an unaddressed IBO slot is a thing no Mesa driver emits) and the
; unused texture slot 0 falls in the texture table, where Gallium's own
; dummy_view is a zero descriptor too.
;
;   llvm-as tex_copy.ll -o tex_copy.bc
;   air2nir tex_copy.bc                               (the NIR)
;   air2nir --interface tex_copy.bc                   (what the runtime binds)
;   ir3_compiler --chip-id 0x06030500 tex_copy.bc     (the Adreno listing)

source_filename = "tex_copy"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

; The signatures follow Apple's own declarations as the corpus carries them
; (read off a .metallib of the device image with `air2nir --ll`, names only):
;   { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)
;   void @air.write_texture_2d.v4f16(ptr addrspace(1), <2 x i32>, <4 x half>, i32, i32)
; with the `.u` part naming an unsigned result (air.read_texture_2d.i16.u.v4i32
; is in the same library). A texture handle is a pointer in address space 1.
; Arguments after the coordinate: read = (lod, flags), write = (value, lod, flags).
declare { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, <4 x i32>, i32, i32)

define void @tex_copy(ptr addrspace(1) %in, ptr addrspace(1) %out, <2 x i32> %gid) local_unnamed_addr {
  %r = tail call { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1) %in, <2 x i32> %gid, i32 0, i32 0)
  ; field 1 is Metal's sparse-residency byte; nothing reads it
  %v = extractvalue { <4 x i32>, i8 } %r, 0
  tail call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) %out, <2 x i32> %gid, <4 x i32> %v, i32 0, i32 0)
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tex_copy, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<uint, read>", !"air.arg_name", !"in"}
!6 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<uint, write>", !"air.arg_name", !"out"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
