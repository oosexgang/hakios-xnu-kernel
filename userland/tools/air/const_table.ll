; A table the module holds as a constant, read at an index the shader computes, as MercuryPoster's
; Flocon snowflakes read their colour ramps (`rampValues`, 98,000 halves): checked by
; tools/air/wordcheck.sh, thread t writes table[7 - t], half_table[t] widened and an element of a
; constant vector, eight threads.
;
;     constant float table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
;     constant half half_table[8] = { 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5 };
;     constant float4 quad = { 10, 20, 30, 40 };
;     kernel void ct(device float *out [[buffer(0)]], uint t [[thread_position_in_grid]]) {
;         out[t] = table[7 - t]; out[8 + t] = half_table[t]; out[16 + t] = quad[t & 3]; }
source_filename = "ct"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

@table = internal addrspace(2) constant [8 x float] [float 1.0, float 2.0, float 3.0, float 4.0, float 5.0, float 6.0, float 7.0, float 8.0], align 4
@quad = internal addrspace(2) constant <4 x float> <float 10.0, float 20.0, float 30.0, float 40.0>, align 16
@half_table = internal addrspace(2) constant [8 x half] [half 0xH3800, half 0xH3E00, half 0xH4100, half 0xH4300, half 0xH4480, half 0xH4580, half 0xH4680, half 0xH4780], align 2

define void @ct(ptr addrspace(1) %0, i32 %t) local_unnamed_addr {
  %r = sub i32 7, %t
  %ri = zext i32 %r to i64
  %p = getelementptr inbounds [8 x float], ptr addrspace(2) @table, i64 0, i64 %ri
  %v = load float, ptr addrspace(2) %p, align 4
  %ti = zext i32 %t to i64
  %o = getelementptr inbounds float, ptr addrspace(1) %0, i64 %ti
  store float %v, ptr addrspace(1) %o, align 4
  %hp = getelementptr inbounds [8 x half], ptr addrspace(2) @half_table, i64 0, i64 %ti
  %h = load half, ptr addrspace(2) %hp, align 2
  %hf = fpext half %h to float
  %t8 = add i64 %ti, 8
  %o2 = getelementptr inbounds float, ptr addrspace(1) %0, i64 %t8
  store float %hf, ptr addrspace(1) %o2, align 4
  %t3 = and i64 %ti, 3
  %qp = getelementptr inbounds float, ptr addrspace(2) @quad, i64 %t3
  %q = load float, ptr addrspace(2) %qp, align 4
  %t16 = add i64 %ti, 16
  %o3 = getelementptr inbounds float, ptr addrspace(1) %0, i64 %t16
  store float %q, ptr addrspace(1) %o3, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ct, !4, !5}
!4 = !{}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
