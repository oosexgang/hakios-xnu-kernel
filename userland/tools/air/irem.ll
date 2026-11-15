; A signed remainder at a divisor the shader computes, as VFX's lightfield_frag takes one, checked by
; tools/air/wordcheck.sh: thread t writes (t - 4) % (3 + t / 8), the sign that of the dividend.
;
;     kernel void ir(device int *out [[buffer(0)]], uint t [[thread_position_in_grid]]) {
;         out[t] = (int(t) - 4) % int(3 + t / 8); }
source_filename = "ir"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define void @ir(ptr addrspace(1) %0, i32 %t) local_unnamed_addr {
  %a = sub i32 %t, 4
  %q = lshr i32 %t, 3
  %d = add i32 %q, 3
  %r = srem i32 %a, %d
  %ti = zext i32 %t to i64
  %o = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %ti
  store i32 %r, ptr addrspace(1) %o, align 4
  ret void
}

!air.kernel = !{!3}
!air.version = !{!1}
!air.language_version = !{!2}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ir, !4, !5}
!4 = !{}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
