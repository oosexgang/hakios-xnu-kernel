; fill_id, the AIR side: the same kernel as tools/kernels/fill_id.asm, written as
; the LLVM IR a Metal compiler would emit for
;
;     kernel void fill_id(device uint *out        [[buffer(0)]],
;                         constant uint &c        [[buffer(1)]],
;                         uint gid                [[thread_position_in_grid]])
;     {
;       out[gid] = gid * 0x9E3779B1u + c;
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. Only the SHAPE of the
; metadata is Apple's convention (the air.kernel node, the per-argument nodes,
; the version nodes), which is what the AIR front end reads; the values are
; this kernel's own.
;
; The two paths meet on the buffer contents: out[gid] must come out identical
; whether the Adreno ran the words of fill_id.asm or the words ir3 compiled
; from this file. What DIFFERS is everything around the words -- see
; tests/fixtures/fill_id.hpp against tests/fixtures/fill_id_air.hpp.
;
;   llvm-as tools/kernels/fill_id.ll -o fill_id.bc
;   air2nir fill_id.bc                               (the NIR)
;   ir3_compiler --chip-id 0x06030500 fill_id.bc     (the Adreno listing)

source_filename = "fill_id"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

; addrspace(1) is Metal's `device`, addrspace(2) its `constant`: the front end
; turns the first into an SSBO at the buffer index and the second into a UBO at
; the buffer index (air_to_nir.c, the air.address_space switch).
define void @fill_id(ptr addrspace(1) %out, ptr addrspace(2) %c, i32 %gid) local_unnamed_addr {
  %cv = load i32, ptr addrspace(2) %c, align 4
  ; 0x9E3779B1 is odd, so gid -> gid*K is a bijection on 32-bit values: no two
  ; invocations can write the same word, which is what makes a readback of the
  ; buffer a check of WHICH invocation wrote each slot and not only that one did.
  %m = mul i32 %gid, -1640531535
  %v = add i32 %m, %cv
  %t = zext i32 %gid to i64
  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t
  store i32 %v, ptr addrspace(1) %po, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @fill_id, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
