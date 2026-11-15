; The other of tools/air/vft.ll's linked functions: x * 10.
source_filename = "mul10"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-darwin16.5.0"

define i32 @mul10(i32 %x) {
  %r = mul i32 %x, 10
  ret i32 %r
}

!air.version = !{!1}
!1 = !{i32 2, i32 5, i32 0}
