; tri_cloop_fs, the AIR side: a fragment function whose constant buffer holds both what decides its control flow and
; what is only computed with, written as the LLVM IR a Metal compiler would emit for
;
;     struct S { uint n; uint pad[3]; float4 a; };
;     fragment float4 tri_cloop_fs(constant S &s [[buffer(0)]])
;     {
;       float4 acc = 0;
;       for (uint i = 0; i < s.n; i++) acc += s.a;
;       return acc;
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention (as tri_cflat_fs).
;
; Why it exists: CoreAnimation's uber shaders read a small constant buffer in which some bytes choose the path through
; thousands of instructions and others are colours, opacities and sizes that change every frame of an animation. A
; specialisation that folds every byte is one compile for every frame's values; one that folds only the bytes control
; flow depends on (here n, bytes 0 to 3: the loop's bound) is one compile for each path, the rest read as before (a,
; bytes 16 to 31). compiler_test: a_buffers_control_bytes_are_those_its_control_flow_reads.

source_filename = "tri_cloop_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_cloop_fs(ptr addrspace(2) %s) local_unnamed_addr {
entry:
  %n = load i32, ptr addrspace(2) %s, align 16
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
  %acc = phi <4 x float> [ zeroinitializer, %entry ], [ %acc1, %body ]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit

body:
  %pa = getelementptr inbounds i8, ptr addrspace(2) %s, i64 16
  %a = load <4 x float>, ptr addrspace(2) %pa, align 16
  %acc1 = fadd <4 x float> %acc, %a
  %i1 = add i32 %i, 1
  br label %loop

exit:
  ret <4 x float> %acc
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_cloop_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"S", !"air.arg_name", !"s"}
