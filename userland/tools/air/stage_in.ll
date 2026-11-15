; tools/air/stageincheck.sh: a compute kernel with a [[stage_in]] float3, read per thread from a buffer
source_filename = "si"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"
; out[gid] = position.x + position.y * 10 + position.z * 100, position the thread's [[stage_in]] float3
define void @si(<3 x float> %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %x = extractelement <3 x float> %0, i64 0
  %y = extractelement <3 x float> %0, i64 1
  %z = extractelement <3 x float> %0, i64 2
  %y10 = fmul float %y, 1.000000e+01
  %z100 = fmul float %z, 1.000000e+02
  %s = fadd float %x, %y10
  %t = fadd float %s, %z100
  %w = zext i32 %2 to i64
  %p = getelementptr inbounds float, ptr addrspace(1) %1, i64 %w
  store float %t, ptr addrspace(1) %p, align 4
  ret void
}
!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @si, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.stage_in", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float3", !"air.arg_name", !"position"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
