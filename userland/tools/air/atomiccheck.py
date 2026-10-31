#!/usr/bin/env python3
"""Check the atomics with a kernel written for the purpose.

docs/air.md said what was missing and why none of Apple's kernels could supply
it: "a sum written by threads that never raced looks exactly like a sum written
by threads that did. Detecting a lost update needs the expected total to depend
on every thread having contributed -- which needs a kernel whose arithmetic is
known, i.e. one written for the purpose rather than one of Apple's." This is
that kernel.

It is generated as AIR, assembled with llvm-as and dispatched by air_run, which
runs 4x4 threads in one workgroup. Every thread does the same five things to two
device buffers whose initial bytes this script chooses (AIR_RUN_BUFFER).

What each part can show:

  [0] += 1 by every thread                  the total is the thread count
  [1] += the value loaded from [4]          atomic load returns what is there
  [2] = max(thread index)                   signed max, and the right operand
  [3] = min(thread index - 100)             signed min across negatives, which
                                            is where an unsigned min would
                                            answer 0xffffff9c-as-huge instead
  buf1[i] = old*16 + i + 1                  see below

The last one is the only part that can detect a lost update, and it does so
without depending on the order the threads ran in. If fetch_add is atomic, the
sixteen values it returned are the numbers 0..15 in some order -- a permutation
-- because each thread saw a different predecessor count. If two threads read
the same old value, the permutation has a duplicate and a gap, and the gap says
which increment was lost. Serial execution also produces a permutation, so this
passes on software by being correct rather than by being unopposed; what it
cannot do is *prove* atomicity on a machine that never ran the threads at once.
The +1 is so that a slot no thread wrote cannot look like a result, and the
*16 + i is so that a thread writing another thread's slot does not read as
correct either.

The operations tested are only the ones the corpus proves exist, read off its
own declarations: global add.s, load, max.s, min.s and store. air_intrinsics.c
also handles and, or, xor and xchg, and those are not tested here, because the
names Apple would emit for them are a guess and a test of a guessed name is a
test of the guess.

usage: atomiccheck.py [--keep DIR]
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"),
)

THREADS = 16          # air_run dispatches GRID x GRID = 4x4 in one workgroup
LOADED = 7            # what sits in [4] for every thread to read

KERNEL = r'''source_filename = "atomics"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare i32 @air.atomic.global.add.s.i32(ptr addrspace(1), i32, i32, i32, i1) local_unnamed_addr
declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1) local_unnamed_addr
declare i32 @air.atomic.global.max.s.i32(ptr addrspace(1), i32, i32, i32, i1) local_unnamed_addr
declare i32 @air.atomic.global.min.s.i32(ptr addrspace(1), i32, i32, i32, i1) local_unnamed_addr
declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1) local_unnamed_addr

define void @atomics(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  ; [0] += 1: the returned old value is this thread's place in the queue
  %c0 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 0
  %old = call i32 @air.atomic.global.add.s.i32(ptr addrspace(1) %c0, i32 1, i32 0, i32 2, i1 true)

  ; [1] += whatever [4] holds, loaded atomically
  %c4 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 4
  %got = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %c4, i32 0, i32 2, i1 true)
  %c1 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 1
  %sum = call i32 @air.atomic.global.add.s.i32(ptr addrspace(1) %c1, i32 %got, i32 0, i32 2, i1 true)

  ; [2] = max(thread index), [3] = min(thread index - 100)
  %c2 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 2
  %mx = call i32 @air.atomic.global.max.s.i32(ptr addrspace(1) %c2, i32 %2, i32 0, i32 2, i1 true)
  %neg = sub i32 %2, 100
  %c3 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 3
  %mn = call i32 @air.atomic.global.min.s.i32(ptr addrspace(1) %c3, i32 %neg, i32 0, i32 2, i1 true)

  ; buf1[index] = old * 16 + index + 1, stored atomically
  %t0 = mul i32 %old, 16
  %t1 = add i32 %t0, %2
  %t2 = add i32 %t1, 1
  %w = zext i32 %2 to i64
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %1, i64 %w
  call void @air.atomic.global.store.i32(ptr addrspace(1) %p1, i32 %t2, i32 0, i32 2, i1 true)
  ret void
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.kernel = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @atomics, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"counters"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"queue"}
!8 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
'''


# The threadgroup atomics and the barrier, which the kernel above cannot reach:
# they go through nir_shared_atomic and a different path for the memory.
#
# Threadgroup memory is not zeroed at dispatch -- neither Metal nor Mesa
# promises that -- so it has to be initialised, and every thread storing 0 to
# the same two words does it without needing a branch.
#
# Then every thread adds 1 to one word and 1 << its own index to the other, and
# after the second barrier reads both back and writes them to its own slot of
# two device buffers. So each of the sixteen slots must hold 16 and 0xffff:
#
#   a lost add leaves a bit clear in every slot, and the bit names the thread
#   whose increment went missing;
#   a barrier that did not wait leaves a *smaller* number in the slot of the
#   thread that ran ahead, and names that one instead.
LOCAL_KERNEL = r"""source_filename = "local_atomics"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@tg = internal addrspace(3) global [2 x i32] undef

declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i1) local_unnamed_addr
declare i32 @air.atomic.local.load.i32(ptr addrspace(3), i32, i32, i1) local_unnamed_addr
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1) local_unnamed_addr
declare void @air.wg.barrier(i32, i32) local_unnamed_addr

define void @local_atomics(ptr addrspace(1) %0, ptr addrspace(1) %1, i32 %2) local_unnamed_addr {
  %t0 = getelementptr inbounds [2 x i32], ptr addrspace(3) @tg, i64 0, i64 0
  %t1 = getelementptr inbounds [2 x i32], ptr addrspace(3) @tg, i64 0, i64 1

  ; every thread stores 0 to both: idempotent, so no branch is needed to make
  ; one thread the initialiser
  call void @air.atomic.local.store.i32(ptr addrspace(3) %t0, i32 0, i32 0, i32 1, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %t1, i32 0, i32 0, i32 1, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)

  %bit = shl i32 1, %2
  %o0 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %t0, i32 1, i32 0, i32 1, i1 true)
  %o1 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %t1, i32 %bit, i32 0, i32 1, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)

  %v0 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %t0, i32 0, i32 1, i1 true)
  %v1 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %t1, i32 0, i32 1, i1 true)
  %w = zext i32 %2 to i64
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %w
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %1, i64 %w
  store i32 %v0, ptr addrspace(1) %p0, align 4
  store i32 %v1, ptr addrspace(1) %p1, align 4
  ret void
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.kernel = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @local_atomics, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"counts"}
!7 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"masks"}
!8 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"""


def initial_bytes():
    """What both buffers start as. Every word is chosen, none is left to the
    harness's pattern, so a slot nothing wrote reads as something invalid
    rather than as something plausible."""
    words = [0] * 16
    words[0] = 0               # the counter
    words[1] = 0               # the sum of the loaded values
    words[2] = 0               # max starts at 0; every thread index is >= 0
    words[3] = 2147483647      # min starts at INT_MAX
    words[4] = LOADED          # what every thread loads
    return b"".join(struct.pack("<i", w) for w in words)


def parse(out):
    """{buffer index: [words]} from air_run's readback."""
    bufs = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r"air_run: device buffer (\d+) after the dispatch", line)
        if m:
            cur = int(m.group(1))
            bufs[cur] = []
            continue
        if cur is not None:
            m = re.match(r"\s*\[\s*(\d+)\]\s+(\d+)\s+0x([0-9a-f]+)", line)
            if m:
                bufs[cur].append(int(m.group(3), 16))
            elif line.startswith("air_run:") and "device buffer" not in line:
                cur = None
    return bufs


def signed(u):
    return u - (1 << 32) if u >> 31 else u


def run_kernel(d, stem, source, data, env_extra=None):
    """Assemble one kernel, dispatch it, and return (buffers, screen, output)."""
    ll, bc, bin_ = (os.path.join(d, stem + e) for e in (".ll", ".bc", ".bin"))
    with open(ll, "w") as f:
        f.write(source)
    with open(bin_, "wb") as f:
        f.write(data)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, None, "llvm-as: " + q.stderr.decode().strip()
    q = subprocess.run([AIR_RUN, bc], capture_output=True,
                       env=dict(os.environ, AIR_RUN_BUFFER=bin_))
    out = q.stdout.decode() + q.stderr.decode()
    screen = "an unnamed screen"
    m = re.search(r"air_run: \S+ on (.+?), as a compute kernel", out)
    if m:
        screen = m.group(1)
    return parse(out), screen, out


def check_local(d):
    """The threadgroup atomics and the barrier. Returns a list of complaints."""
    # the initial bytes do not matter here: every slot is written by its own
    # thread and the threadgroup words are initialised by the kernel
    bufs, screen, out = run_kernel(d, "local", LOCAL_KERNEL,
                                   b"\0" * 64)
    if bufs is None or 0 not in bufs or 1 not in bufs:
        tail = "\n      ".join((out or "").strip().splitlines()[-5:])
        return ["the threadgroup kernel did not read both buffers back:\n      " + tail]
    bad = []
    for i in range(THREADS):
        if bufs[0][i] != THREADS:
            bad.append("thread %-2d saw %d of %d increments: the barrier did not wait"
                       % (i, bufs[0][i], THREADS))
        want = (1 << THREADS) - 1
        if bufs[1][i] != want:
            missing = [b for b in range(THREADS) if not bufs[1][i] & (1 << b)]
            bad.append("thread %-2d saw mask 0x%08x, want 0x%08x: nothing from thread%s %s"
                       % (i, bufs[1][i], want, "" if len(missing) == 1 else "s",
                          ", ".join(str(b) for b in missing)))
    return bad


def main(argv):
    keep = None
    if len(argv) >= 2 and argv[0] == "--keep":
        keep = argv[1]
    if not os.path.exists(AIR_RUN):
        print("atomiccheck: no air_run at %s (run tools/air/mk-host.sh)" % AIR_RUN,
              file=sys.stderr)
        return 1

    d = keep or tempfile.mkdtemp(prefix="atomiccheck.")
    os.makedirs(d, exist_ok=True)

    bufs, screen, out = run_kernel(d, "atomics", KERNEL, initial_bytes())
    if bufs is None or 0 not in bufs or 1 not in bufs:
        print("atomiccheck: the dispatch did not read both buffers back:")
        for line in (out or "").strip().splitlines()[-6:]:
            print("  " + line)
        return 1

    c, queue = bufs[0], bufs[1]
    bad = []

    def want(label, got, exp):
        if got != exp:
            bad.append("%-34s got %-12s want %s" % (label, got, exp))

    want("[0] threads that incremented", c[0], THREADS)
    want("[1] sum of the loaded value", c[1], THREADS * LOADED)
    want("[2] max of the thread indices", c[2], THREADS - 1)
    want("[3] min of index - 100", signed(c[3]), -100)
    want("[4] the loaded value, untouched", c[4], LOADED)

    # the permutation: every slot written by its own thread, and the old values
    # the sixteen fetch_adds returned being 0..15 with nothing repeated
    olds = []
    for i in range(THREADS):
        v = queue[i]
        if v == 0:
            bad.append("queue[%-2d] no thread wrote it" % i)
            continue
        if (v - 1) % 16 != i:
            bad.append("queue[%-2d] was written by thread %d" % (i, (v - 1) % 16))
            continue
        o = (v - 1) // 16
        if not 0 <= o < THREADS:
            # the whole set being shifted is what returning the new value
            # instead of the old looks like, so say that rather than leave it
            # to be read off a missing element
            bad.append("queue[%-2d] was given %d, which is not a predecessor count "
                       "of %d threads" % (i, o, THREADS))
        olds.append(o)
    if len(olds) == THREADS and sorted(olds) != list(range(THREADS)):
        seen = {}
        for i, o in enumerate(olds):
            seen.setdefault(o, []).append(i)
        for o in sorted(seen):
            if len(seen[o]) > 1:
                bad.append("fetch_add returned %d to threads %s: a lost update"
                           % (o, ", ".join(str(t) for t in seen[o])))
        for o in range(THREADS):
            if o not in seen:
                bad.append("no thread was given %d: that increment went missing" % o)

    local_bad = check_local(d)
    bad += local_bad

    for line in bad:
        print("WRONG   " + line)
    print()
    if bad:
        print("atomiccheck: %d of the checks failed on %s" % (len(bad), screen))
        return 1
    print("atomiccheck: the eight atomic operations the corpus uses agree, on %s" % screen)
    print("atomiccheck: five on device memory, three on threadgroup memory across two")
    print("atomiccheck: barriers, every thread seeing all sixteen contributions")
    print("atomiccheck: %d threads each incremented once and were given a distinct"
          % THREADS)
    print("atomiccheck: predecessor count, 0 to %d, so no update was lost" % (THREADS - 1))
    if "softpipe" in screen or "software" in screen:
        print("atomiccheck: the two barriers are really under test here: softpipe advances its")
        print("atomiccheck: invocations in groups of four, so with the barrier made a no-op")
        print("atomiccheck: every thread sees 4 of 16 increments and this says so.")
        print("atomiccheck: ATOMICITY still is not tested. Softpipe applies a group's atomic")
        print("atomiccheck: lane by lane, so it cannot lose an update however the lowering is")
        print("atomiccheck: written. That needs the GPU, where sixteen threads really do race.")
    if keep:
        print("atomiccheck: kept in %s" % d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
