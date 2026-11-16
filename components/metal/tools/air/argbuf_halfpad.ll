; A value set whose struct the compiler pads with one lone `i8`, not an array of
; them: the byte between a bool and a half, the shape of VFX's
; external_entity_constants (bt_ui_breakthrough_enabled at byte 36, the influence
; half at 38). Dispatched by tools/air/argbufvalcheck.c.
;
;     struct Pad {                         // constant, 8 bytes
;         float f  [[id(0)]];              // byte 0
;         bool  on [[id(1)]];              // byte 4, then one byte of padding
;         half  h  [[id(2)]];              // byte 6
;     };
;     kernel void ap(constant Pad &p [[buffer(0)]], device float *out [[buffer(1)]],
;                    uint gid [[thread_position_in_grid]]) {
;         out[gid] = p.on ? p.f + float(p.h) + gid : -1.0f;
;     }
;
source_filename = "ap"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

%struct.Pad = type { float, i8, i8, half }
define void @ap(ptr addrspace(2) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %pf = getelementptr inbounds %struct.Pad, ptr addrspace(2) %0, i64 0, i32 0
  %f = load float, ptr addrspace(2) %pf, align 4
  %po = getelementptr inbounds %struct.Pad, ptr addrspace(2) %0, i64 0, i32 1
  %on = load i8, ptr addrspace(2) %po, align 4
  %ph = getelementptr inbounds %struct.Pad, ptr addrspace(2) %0, i64 0, i32 3
  %h = load half, ptr addrspace(2) %ph, align 2
  %hf = fpext half %h to float
  %g = uitofp i32 %2 to float
  %s1 = fadd float %f, %hf
  %s2 = fadd float %s1, %g
  %b = icmp ne i8 %on, 0
  %r = select i1 %b, float %s2, float -1.0
  %w = zext i32 %2 to i64
  %pw = getelementptr inbounds float, ptr addrspace(1) %1, i64 %w
  store float %r, ptr addrspace(1) %pw, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ap, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !20, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Pad", !"air.arg_name", !"p"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!20 = !{i32 0, i32 4, i32 0, !"float", !"f", !"air.indirect_argument", !21, i32 4, i32 1, i32 0, !"bool", !"on", !"air.indirect_argument", !22, i32 6, i32 2, i32 0, !"half", !"h", !"air.indirect_argument", !23}
!21 = !{i32 0, !"air.indirect_constant", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"f"}
!22 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"bool", !"air.arg_name", !"on"}
!23 = !{i32 2, !"air.indirect_constant", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"half", !"air.arg_name", !"h"}
