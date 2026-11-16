; A binary search that steps its pointer with `select`, and a struct carried round the same loop:
; the two shapes of RenderBox's uber shaders (uber_vertex over a path's cubic segments, uber_fragment's
; colour state) that the frontend refused -- "a phi of pointers one of whose operands is itself a pointer
; chosen at run time" and "a phi of a struct, which NIR has no register for". Hand-written, not Apple's.
;
;     struct Seg { int key; int val; };
;     kernel void ptrsel_loop(device const Seg *segs [[buffer(0)]], device int *out [[buffer(1)]],
;                             uint gid [[thread_position_in_grid]])
;     {
;         int target = int(gid) * 5, len = segs[0].key;      // segs[0] is the count, segs[1..] the table
;         device const Seg *p = segs + 1;
;         struct { int steps; int sum; } acc = { 0, 0 };
;         do { int half = len >> 1; device const Seg *mid = p + half;
;              bool after = mid->key > target;
;              len = after ? half : len - half; p = after ? p : mid;        // the select of two pointers
;              acc.steps += 1; acc.sum += mid->key;                         // the struct round the loop
;         } while (len > 1);
;         out[gid] = p->val + 1000 * acc.steps + 100000 * (acc.sum % 97);
;     }
;
; Run on this machine (softpipe), with the table in buffer 0 -- count 8, keys 10..80, values 101..108:
;
;   llvm-as tools/air/ptrsel_loop.ll -o /tmp/ptrsel_loop.bc
;   air_run --block 16,1,1 --buffer 0@0=0800000000000000 \
;     --buffer 0@8=0a00000065000000140000006600000023000000670000002800000068000000 \
;     --buffer 0@40=3200000069000000390000006a000000460000006b000000500000006c000000 /tmp/ptrsel_loop.bc
;
; Every output must be what tools/air/ptrsel_loop.py computes for its thread; a select taken as the other
; side, an offset carried wrongly round the loop, or a struct member lost on an edge each gives other
; numbers (the keys are not evenly spaced, so the steps and sums differ between threads).

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%struct.Seg = type { i32, i32 }
%struct.Acc = type { i32, i32 }

define void @ptrsel_loop(ptr addrspace(1) %segs, ptr addrspace(1) %out, i32 %gid) local_unnamed_addr {
entry:
  %target = mul i32 %gid, 5
  %len0 = load i32, ptr addrspace(1) %segs, align 4
  %p0 = getelementptr inbounds %struct.Seg, ptr addrspace(1) %segs, i64 1
  br label %loop

loop:
  %p = phi ptr addrspace(1) [ %p0, %entry ], [ %pn, %loop ]
  %len = phi i32 [ %len0, %entry ], [ %lenn, %loop ]
  %acc = phi %struct.Acc [ zeroinitializer, %entry ], [ %acc2, %loop ]
  %half = lshr i32 %len, 1
  %halfz = zext i32 %half to i64
  %mid = getelementptr inbounds %struct.Seg, ptr addrspace(1) %p, i64 %halfz
  %midkey = load i32, ptr addrspace(1) %mid, align 4
  %after = icmp sgt i32 %midkey, %target
  %rest = sub nsw i32 %len, %half
  %lenn = select i1 %after, i32 %half, i32 %rest
  %pn = select i1 %after, ptr addrspace(1) %p, ptr addrspace(1) %mid
  %steps = extractvalue %struct.Acc %acc, 0
  %sum = extractvalue %struct.Acc %acc, 1
  %steps2 = add i32 %steps, 1
  %sum2 = add i32 %sum, %midkey
  %acc1 = insertvalue %struct.Acc %acc, i32 %steps2, 0
  %acc2 = insertvalue %struct.Acc %acc1, i32 %sum2, 1
  %more = icmp sgt i32 %lenn, 1
  br i1 %more, label %loop, label %done

done:
  %valp = getelementptr inbounds %struct.Seg, ptr addrspace(1) %pn, i64 0, i32 1
  %val = load i32, ptr addrspace(1) %valp, align 4
  %s = extractvalue %struct.Acc %acc2, 0
  %m = extractvalue %struct.Acc %acc2, 1
  %s1000 = mul i32 %s, 1000
  %mm = urem i32 %m, 97
  %m1e5 = mul i32 %mm, 100000
  %r0 = add i32 %val, %s1000
  %r = add i32 %r0, %m1e5
  %gidz = zext i32 %gid to i64
  %o = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %gidz
  store i32 %r, ptr addrspace(1) %o, align 4
  ret void
}

!air.kernel = !{!14}
!air.version = !{!9}
!air.language_version = !{!10}

!9 = !{i32 2, i32 5, i32 0}
!10 = !{!"Metal", i32 3, i32 0, i32 0}
!14 = !{ptr @ptrsel_loop, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Seg", !"air.arg_name", !"segs"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
