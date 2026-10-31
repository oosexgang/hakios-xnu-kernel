; A constant on a phi edge, shared with a use after the branch: the shape that
; lost MetalFaceMask_AddSeedPoint's `-1` (docs/air.md section 5d).
;
; Hand-written, not Apple's. Assemble and translate:
;
;   llvm-as tools/air/phiconst.ll -o /tmp/phiconst.bc
;   build/mesa-host/build/src/compiler/air/air2nir /tmp/phiconst.bc
;
; What to look for is the phi in the block after the `if`. It must read
;
;   16 %9 = phi b1: %8, b2: %1 (0xffff)
;
; and not `b2: %0` with `%0 = undefined`. The -1 is the same constant as the
; compare below it uses, so before the fix it was built inside b1 -- a def that
; does not dominate the edge from b2 -- and nir_repair_ssa, which
; nir_lower_goto_ifs runs, repaired that into undef on the path through b2.
;
; Spelling the edge's constant `-2` instead makes it a constant nothing else
; uses, and it survived even before the fix. That is what says the sharing and
; not the width is the cause, so change it and watch the undef come back if the
; two are ever confused again.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @phiconst(ptr addrspace(1) %0, ptr addrspace(1) %1) local_unnamed_addr {
  %3 = load i32, ptr addrspace(1) %0, align 4
  %4 = icmp ult i32 %3, 4
  br i1 %4, label %5, label %7

5:
  %6 = load i16, ptr addrspace(1) %1, align 2
  br label %7

7:
  %8 = phi i16 [ %6, %5 ], [ -1, %2 ]
  %9 = icmp eq i16 %8, -1
  br i1 %9, label %12, label %10

10:
  store i16 %8, ptr addrspace(1) %1, align 2
  br label %12

12:
  ret void
}

!air.kernel = !{!14}
!air.version = !{!9}
!air.language_version = !{!10}

!9 = !{i32 2, i32 5, i32 0}
!10 = !{!"Metal", i32 3, i32 0, i32 0}
!14 = !{ptr @phiconst, !15, !16}
!15 = !{}
!16 = !{!17, !18}
!17 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"count"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"ushort", !"air.arg_name", !"ids"}
