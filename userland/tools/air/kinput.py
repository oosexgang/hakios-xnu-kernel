#!/usr/bin/env python3
"""What to put in the buffers so that the kernels whose loops read control data end.

`tools/air/kstruct.py` sorts the corpus's 47 kernels by what they do with their
buffers and names 14 whose loop leaves on a value *loaded from* a buffer. For
those, `(i * 7 + 13) & 0xff` -- or zeros, or `0xff` -- is not an input: the
value the loop compares against is a count, an index, or a sentinel, and the
pattern contains none of them. docs/air.md section 5 records what one such
dispatch costs: `gpuWait` latches after a single timeout, every later wait
returns false at once, so every dispatch after it reads as "finished and
empty", and the next boot panics in the GPU ramp. That is why
`userland/rc.jobs.metalcompute` leaves seven of the sixteen buffer-only kernels
out by name.

This builds the missing input. For each kernel it says which loop does not end,
which value its exit test compares against, which buffer and byte offset that
value is loaded from, and what to write there instead -- and then writes it, as
bytes a harness can put in the buffer.

The two loops that were measured hanging are not infinite, and saying so
precisely matters, because the fix differs. `fbs_bistochastize_iter` counts from
one HashEntry field to another under an `icmp ult` guard, so it runs their
difference and then stops; the pattern makes that difference 2.9 billion. The
two producer kernels scan x from `bounds.x` to `bounds.z` of a constant buffer,
which the pattern makes 943 million pixels apart, with a texture read each time
round. Both numbers are printed at the end of this tool's own table, computed
from the same fill, so they can be checked rather than quoted -- and they
correct docs/air.md section 5, which says of the first "runs about four billion
times whenever the second is smaller". Backwards: when the second is smaller the
guard means it is not entered at all. The MetalFaceMask chain walks are the ones
that genuinely do not end -- they leave only on a next-id of -1, and a chain of
arbitrary bytes is a cycle.

Every input here was then EXECUTED against the kernel's own module on this
machine, which is the one check a reasoned argument cannot make for itself. The
modules air2nir --ll prints are LLVM IR: with the air64 triple and datalayout
dropped, the address spaces flattened, host bodies given to the air.* calls
(the obvious ones, and airdefs.py's reading where it has one) and a driver that
mirrors this harness -- sixteen zeroed 4096-byte buffers and sixteen threads
whose scalar thread_position_in_grid is x -- clang links the kernel into a
program and it runs under a timeout. What that settles is termination and the
control flow; what it cannot settle is the hardware, our NIR (this runs Apple's
LLVM, not our translation of it) or a race (the threads run one after another).
Three things make the result mean something:

  the fill that hangs      with the old pattern in every buffer, every one of
                           these kernels dies on an out-of-bounds access within
                           seconds -- the chain index and the scan bound are
                           both nonsense, and on a host that is a SIGSEGV where
                           on a GPU it is a hang.
  a deliberate cycle       ConnectSpans given this input with each span's
                           nextSpanID pointing at itself instead of -1 runs
                           until the timeout kills it. So "it returned" is a
                           measurement and not a default.
  the recorded runs        AddSeedPoint and DrawSpans return on zeros and
                           fbs_bistochastize_iter returns on zeros, which is
                           what the device measured (docs/air.md section 5).
                           ConnectSpans on zeros returns here too, and the
                           device is recorded as hanging on it three boots out
                           of three -- see the note on that kernel.

What this tool does NOT do is guess. Where the meaning of a buffer could not be
established from the module and from Apple's own `air.arg_name` /
`air.struct_type_info`, the row says so and the kernel is marked DO NOT RUN. A
hung GPU costs a boot and the next run's trust; an honest gap costs a line.

Every offset written here is checked against the library's own
`air.struct_type_info`, and every buffer against the `air.arg_name` and address
space Apple declares for it, before anything is printed; every kernel is
checked to be one kstruct still calls endless, so this file and that one cannot
drift apart. Nothing is copied out of the corpus: the bytes below are values
derived here, with the reason beside each one.

usage: kinput.py [--hex] [--jobs] [KERNEL...]
       (no arguments)  the table: every byte, its offset, and why
       --hex           the `--buffer N@OFF=HEX` arguments a device test consumes
       --jobs          whole command lines, in rc.jobs order
"""

import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kstruct  # noqa: E402  (the classifier that named these kernels, so the two agree)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))

LIB = "ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib"
ON_DEVICE = "/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib"

# The harness this input is built for: userland/src/metal_drv_test.m makes
# sixteen buffers of 4096 bytes, memsets them to zero and binds buffer i at
# index i; it dispatches one threadgroup of 4x4x1. Both numbers are load
# bearing and neither is ours to choose here:
#
#   4096 is why no kernel below is given an association. MetalFaceMaskSpanTable_t
#   puts `associations` at byte 917492 of a 1179628-byte struct, so the first
#   association a kernel touches is 900 KiB past the end of the buffer. Every
#   table built here therefore has associationCount = 0, and the kernel whose
#   work IS the associations is reported as unmeasurable rather than fed
#   something that would fault. air_run has the same limit for a different
#   reason: it sizes a buffer from `air.buffer_size`, which these arguments do
#   not carry, so it also binds 4096 (air_run.c, dispatch_kernel).
#
#   4x4x1 is why every table has four rows. A scalar `thread_position_in_grid`
#   becomes the x component of the global invocation id (air_to_nir.c,
#   AIR_SV_THREAD_POS_GRID, trimmed by `fit`), so the ids a kernel actually
#   sees are 0, 1, 2 and 3 -- each of them by four threads, since y also runs
#   0..3. Four threads per id means every write below had to be checked for
#   being either idempotent or thread-private; where it could not be, the input
#   was changed until it was. A kernel given more rows than the grid has ids
#   would leave them alone, but one given FEWER would read a row that does not
#   exist, so the two numbers have to match.
HARNESS_BYTES = 4096
GRID = 4

# Apple's own layout, by the names its air.struct_type_info gives, as
# (field, offset, size). Checked against the library below: an offset that
# lives only in a comment is an offset nobody checked.
LAYOUT = {
    "MetalFaceMaskSpanTable_t": [
        ("rowCount", 0, 4), ("firstRowPos", 4, 4), ("spanCount", 8, 4),
        ("associationCount", 12, 4), ("spans", 16, 14), ("associations", 917492, 4)],
    "MetalFaceMaskSpan_t": [
        ("nextSpanID", 0, 2), ("parentSpanID", 2, 2), ("nextRingSiblingID", 4, 2),
        ("startX", 6, 2), ("endX", 8, 2), ("ringCount", 10, 2), ("fillValue", 12, 1)],
    "MetalFaceMaskSpanAssociation_t": [("parentSpanID", 0, 2), ("childSpanID", 2, 2)],
    "MetalFaceMaskSeedPoint_t": [("x", 0, 4), ("y", 4, 4), ("fillValue", 8, 1)],
    "MetalFaceMaskPropogateConfig_t": [
        ("offset", 0, 4), ("minorCount", 4, 4), ("majorStride", 8, 4)],
    "MetalFaceMaskEyeQuads_t": [
        ("nQuads", 0, 4), ("featherDistance", 4, 4), ("fillValue", 8, 1),
        ("quads", 16, 32)],
    "HashEntry": [
        ("key", 0, 4), ("count", 8, 4), ("blur_count", 12, 4), ("blur_n", 16, 4),
        ("blur_offset", 20, 4)],
    "FBSUniforms": [
        ("lambda", 0, 4), ("N", 4, 4), ("dims", 8, 2), ("sigma_s", 12, 4),
        ("sigma_r_luma", 16, 4)],
}

SPAN = dict((f, o) for f, o, _s in LAYOUT["MetalFaceMaskSpan_t"])
TABLE = dict((f, o) for f, o, _s in LAYOUT["MetalFaceMaskSpanTable_t"])
SPAN_STRIDE = dict((f, s) for f, _o, s in LAYOUT["MetalFaceMaskSpanTable_t"])["spans"]
ENTRY_STRIDE = 24         # HashEntry, from its own air.arg_type_size

# How many spans fit in what the harness binds, which bounds everything the two
# producer kernels are allowed to append: span i ends at 16 + 14*i + 13.
SPANS_THAT_FIT = (HARNESS_BYTES - TABLE["spans"]) // SPAN_STRIDE

# air_run prints these when it says what it filled a buffer with; here they are
# only used to say, in iterations, what the old fill asked of these loops.
PATTERN = lambda i: (i * 7 + 13) & 0xff    # noqa: E731


def pattern_word(off):
    return int.from_bytes(bytes(PATTERN(i) for i in range(off, off + 4)), "little")


def f32(x):
    """A float as the four bytes a device buffer holds, so that a value and its
    bits are never written down twice and allowed to disagree."""
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ---------------------------------------------------------------- the module

def module(kernel):
    """Our own reader's view of one kernel: the text air2nir --ll prints."""
    out = subprocess.run([AIR2NIR, "--ll", os.path.join(ROOT, LIB), kernel],
                         capture_output=True)
    return out.stdout.decode(errors="replace")


def md_nodes(ll):
    return dict((int(m.group(1)), m.group(2))
                for m in re.finditer(r"^!(\d+) = (?:distinct )?!\{(.*)\}$", ll, re.M))


# A field of an air.struct_type_info node: offset, size, element count, type,
# name -- optionally preceded by the nested node that describes its own type.
FIELD_RE = re.compile(r'(?:!"air\.struct_type_info", !(\d+), )?'
                      r'i32 (\d+), i32 (\d+), i32 (\d+), !"([^"]+)", !"([^"]+)"')


def struct_fields(nodes, nid):
    """[(name, offset, size, count, type, nested node or None)]."""
    return [(name, int(off), int(size), int(count), ty, int(sub) if sub else None)
            for sub, off, size, count, ty, name in FIELD_RE.findall(nodes[nid])]


def collect_structs(nodes, nid, type_name, into):
    """Every struct layout reachable from one argument, keyed by type name, so
    that a nested type like MetalFaceMaskSpan_t is checked and not assumed."""
    if type_name in into:
        return
    fields = struct_fields(nodes, nid)
    into[type_name] = [(f, o, s) for f, o, s, _c, _t, _n in fields]
    for _f, _o, _s, _c, ty, sub in fields:
        if sub is not None:
            collect_structs(nodes, sub, ty, into)


def arg_table(ll):
    """One row per argument, as Apple's air.kernel metadata declares it."""
    nodes = md_nodes(ll)
    m = re.search(r"^!air\.kernel = !\{!(\d+)\}$", ll, re.M)
    if not m:
        return [], nodes
    ids = re.findall(r"!(\d+)", nodes[int(m.group(1))])
    args = []
    for nid in re.findall(r"!(\d+)", nodes[int(ids[-1])]):
        text = nodes[int(nid)]
        a = {"param": int(re.match(r"i32 (\d+)", text).group(1))}
        for key, as_ in (("air.location_index", "slot"),
                         ("air.address_space", "address_space"),
                         ("air.arg_type_size", "arg_type_size"),
                         ("air.buffer_size", "buffer_size")):
            k = re.search(r'!"%s", i32 (\d+)' % re.escape(key), text)
            if k:
                a[as_] = int(k.group(1))
        for key in ("air.arg_name", "air.arg_type_name"):
            k = re.search(r'!"%s", !"([^"]*)"' % re.escape(key), text)
            if k:
                a[key.split(".")[-1]] = k.group(1)
        a["kind"] = ("buffer" if '!"air.buffer"' in text else
                     "texture" if '!"air.texture"' in text else
                     "sampler" if '!"air.sampler"' in text else "builtin")
        sub = re.search(r'!"air\.struct_type_info", !(\d+)', text)
        a["struct"] = int(sub.group(1)) if sub else None
        args.append(a)
    return args, nodes


# ---------------------------------------------------------------- the bytes

LABEL = {"I": "u32", "i": "i32", "H": "u16", "h": "i16", "B": "u8"}


class Blob(object):
    """One buffer of one kernel: the bytes, and a row per byte range saying why.

    Every write goes through put(), which refuses two writes to the same byte
    and anything past what the harness binds. The rows are as much of the
    deliverable as the bytes are: a hex string nobody can check is a guess with
    better presentation.
    """

    def __init__(self, slot, name, kind, type_name):
        self.slot, self.name, self.kind, self.type_name = slot, name, kind, type_name
        self.bytes = bytearray(HARNESS_BYTES)
        self.rows = []
        self.taken = set()

    def put(self, off, fmt, value, why, shown=None):
        size = struct.calcsize("<" + fmt)
        if off < 0 or off + size > HARNESS_BYTES:
            raise ValueError("%s: byte %d is outside the %d the harness binds"
                             % (self.name, off, HARNESS_BYTES))
        hit = set(range(off, off + size)) & self.taken
        if hit:
            raise ValueError("%s: byte %d written twice" % (self.name, min(hit)))
        struct.pack_into("<" + fmt, self.bytes, off, value)
        self.taken |= set(range(off, off + size))
        if shown is None:
            shown = ("0x%0*x" % (size * 2, value & ((1 << (8 * size)) - 1))
                     if fmt in "HB" else str(value))
        self.rows.append((off, fmt, shown, why))
        return self

    def chunks(self):
        """The written bytes as (offset, bytes) runs, so the hex stays short:
        the harness has already zeroed the buffer, and the flag only has to
        carry what is not zero."""
        out, run = [], None
        for off in sorted(self.taken):
            if run is not None and off == run[0] + len(run[1]):
                run[1].append(self.bytes[off])
            else:
                run = (off, bytearray([self.bytes[off]]))
                out.append(run)
        return [(off, bytes(b)) for off, b in out]

    def args(self):
        return ["--buffer %d@%d=%s" % (self.slot, off, blob.hex())
                for off, blob in self.chunks()]


# ---------------------------------------------------------- the common input

def span_table(slot, rows=GRID, span_count=GRID, spans=True):
    """A span table whose every chain ends after one step.

      rowCount          every walk here is guarded by it: `rowCount > id` or the
                        row index is out of range and the walk is handed the
                        literal -1 instead. GRID, so the ids this dispatch
                        produces each find a row.
      firstRowPos       0, so a row index of `y - firstRowPos` is never
                        negative. DrawEye compares that index with `icmp slt`
                        and then indexes with `sext`, so a negative one reads
                        in front of the buffer.
      spanCount         GRID: the consumers never read it, the producers append
                        from it, and a producer is given 0 instead.
      associationCount  0, and here it cannot be anything else: see the note on
                        HARNESS_BYTES above.
      spans[k]          one span per row. nextSpanID = -1 is the whole point:
                        every walk in this family leaves on `icmp eq i16 _, -1`
                        against a next-id, so -1 at offset +0 of the span is
                        the sentinel that ends it. parentSpanID = k makes each
                        span its own root, which is what makes the one store
                        ConnectSpans and PropagateParents do idempotent.
                        nextRingSiblingID = k is a ring of one. startX/endX
                        give each row a four-pixel run, 10 apart from its
                        neighbour's, so that two rows never overlap -- which is
                        what keeps ConnectSpans out of the branch that appends
                        an association.
    """
    b = Blob(slot, "spanTable", "device", "MetalFaceMaskSpanTable_t")
    # a producer -- the one given spanCount 0 -- never reads these two: it WRITES
    # them at the end, from its own bounds. Saying so keeps the table from
    # claiming a reason it does not have.
    b.put(TABLE["rowCount"], "I", rows,
          "rowCount: %d rows, one per thread_position_in_grid.x of a %dx%d group"
          % (rows, GRID, GRID) if span_count else
          "rowCount: not read by a producer -- it writes this field itself, as "
          "1 - bounds.y + bounds.w")
    b.put(TABLE["firstRowPos"], "i", 0,
          "firstRowPos: row 0 is y 0, so `y - firstRowPos` is never negative"
          if span_count else
          "firstRowPos: not read by a producer -- it writes this field itself, as "
          "bounds.y")
    b.put(TABLE["spanCount"], "i", span_count,
          "spanCount (atomic): %d" % span_count +
          (" spans exist" if span_count else ", so a producer appends from the start"))
    b.put(TABLE["associationCount"], "i", 0,
          "associationCount (atomic): 0. associations[] starts at byte %d of this "
          "struct and the harness binds %d, so any other value reads and writes past "
          "the end" % (TABLE["associations"], HARNESS_BYTES))
    if not spans:
        return b
    for k in range(rows):
        at = TABLE["spans"] + SPAN_STRIDE * k
        x0 = 10 * k
        b.put(at + SPAN["nextSpanID"], "H", 0xffff,
              "spans[%d].nextSpanID = -1: THE SENTINEL every chain walk leaves on" % k)
        b.put(at + SPAN["parentSpanID"], "H", k,
              "spans[%d].parentSpanID = %d, its own root, which makes the one store "
              "ConnectSpans and PropagateParents do a no-op" % (k, k))
        b.put(at + SPAN["nextRingSiblingID"], "H", k,
              "spans[%d].nextRingSiblingID = %d: a ring of one" % (k, k))
        b.put(at + SPAN["startX"], "h", x0, "spans[%d].startX = %d" % (k, x0))
        b.put(at + SPAN["endX"], "h", x0 + 3,
              "spans[%d].endX = %d: a four-pixel run, disjoint from its neighbours'"
              % (k, x0 + 3))
        b.put(at + SPAN["ringCount"], "H", 1, "spans[%d].ringCount = 1" % k)
        b.put(at + SPAN["fillValue"], "B", 0xff,
              "spans[%d].fillValue = 255, so a drawing kernel's `fill != 0` test "
              "passes and it actually writes" % k)
    return b


def first_span_in_rows(slot, rows=GRID):
    """The per-row head span id: entry k is the first span of row k. A row with
    no spans holds -1, which is the other way every walk here ends."""
    b = Blob(slot, "firstSpanInRows", "device", "ushort")
    for k in range(rows):
        b.put(2 * k, "H", k,
              "firstSpanInRows[%d] = %d: the head of row %d's chain" % (k, k, k))
    return b


def int4(slot, name, v, why):
    b = Blob(slot, name, "constant", "int4")
    for i, (x, w) in enumerate(zip(v, why)):
        b.put(4 * i, "i", x, "%s.%s = %d: %s" % (name, "xyzw"[i], x, w))
    return b


def fbs_grid(slot):
    """grid_buf: four HashEntry whose blur_offset is a prefix sum.

    This is the one value that matters. Every fbs_* loop in this group is

        for (i = grid[gid - 1].blur_offset; i != grid[gid].blur_offset; i++)

    guarded by `icmp ult` on the two, so the iteration count is exactly their
    difference and the loop is not entered when that is zero or negative.
    blur_offset[k] = k + 1 makes every thread's count exactly one. The pattern
    makes it an arbitrary 32-bit number, which is the whole of the hang.
    """
    b = Blob(slot, "grid_buf", "device", "HashEntry")
    for k in range(GRID):
        at = ENTRY_STRIDE * k
        b.put(at + 8, "I", 1,
              "grid_buf[%d].count = 1: a weight in the arithmetic, not a bound" % k)
        b.put(at + 20, "I", k + 1,
              "grid_buf[%d].blur_offset = %d: THE BOUND. The loop runs "
              "blur_offset[%d] - blur_offset[%d] = 1 time" % (k, k + 1, k, k - 1))
    return b


def fbs_floats(slot, name, value, why):
    b = Blob(slot, name, "device", "float")
    for k in range(GRID):
        b.put(4 * k, "I", f32(value), "%s[%d] = %g: %s" % (name, k, value, why),
              shown="%g" % value)
    return b


def fbs_bidx(slot):
    b = Blob(slot, "bidx_buf", "device", "uint")
    for k in range(GRID):
        b.put(4 * k, "I", k,
              "bidx_buf[%d] = %d: an index into the float buffers, so it has to be "
              "one that exists -- the pattern's value does not" % (k, k))
    return b


def fbs_params(slot):
    b = Blob(slot, "params", "constant", "FBSUniforms")
    b.put(0, "I", f32(1.0), "lambda = 1: a weight", shown="1")
    b.put(4, "I", GRID,
          "N = %d: the guard is `N > gid`, so a thread with a bigger id returns at "
          "once and this is how many ids the dispatch has" % GRID)
    b.put(8, "H", 1, "dims = 1: a multiplier in the arithmetic, not a bound",
          shown="1")
    b.put(12, "I", f32(1.0), "sigma_s = 1", shown="1")
    b.put(16, "I", f32(1.0), "sigma_r_luma = 1", shown="1")
    return b


# ---------------------------------------------------------------- the kernels
#
# Each entry: the loop that does not end, in the module's own SSA names; where
# its exit value is loaded from; the buffers; what the kernel then does; what
# should come back; and a confidence that is about the claim, not the finding.

HIGH = "HIGH"      # the exit test and the offset it reads are both in the module,
                   # and nothing else about the run is in doubt
MED = "MEDIUM"     # the exit is as certain; something else about the run is not
EMPTY = "RUNS, EMPTY"   # provably safe to dispatch, but the loop is never entered,
                        # so the dispatch measures the harness and not the kernel
NONE = "DO NOT RUN"     # no input established: the row says what is missing

TEXFMT_2D = "--texslots 0=2d:rgba32float"
TEXFMT_CUBE = "--texslots 0=3d:rgba16float,1=2d:rgba32float"


def k_addseedpoint():
    seed = Blob(2, "seedPoint", "constant", "MetalFaceMaskSeedPoint_t")
    seed.put(0, "i", 1, "seedPoint.x = 1, inside spans[0]'s run [0, 3]")
    seed.put(4, "i", 0, "seedPoint.y = 0, so the row is `y - bounds.y` = 0")
    seed.put(8, "B", 0x5a,
             "seedPoint.fillValue = 0x5a: the byte the kernel writes, chosen to differ "
             "from the 0xff already in the span so that the write is visible")
    return dict(
        confidence=HIGH,
        loop="block 34 (back edge from 53); exits on `%56 = icmp eq i16 %55, -1`",
        reads="%%55 is spans[id].nextSpanID -- offset +%d of the %d-byte span at "
              "spanTable+%d+%d*id, buffer 3"
              % (SPAN["nextSpanID"], SPAN_STRIDE, TABLE["spans"], SPAN_STRIDE),
        bufs=[int4(0, "bounds", (0, 0, 63, 63),
                   ("the kernel returns if `x < bounds.x`",
                    "or `y < bounds.y`; the row is `y - bounds.y`",
                    "or `x > bounds.z`", "or `y > bounds.w`")),
              Blob(1, "region", "constant", "int4"),
              seed, span_table(3), first_span_in_rows(4)],
        does="this kernel has NO thread id among its arguments, so all sixteen threads "
             "do the same thing: row 0, head span 0, x = 1 inside [startX, endX], so it "
             "takes the write at block 46 on the first visit and returns.",
        expect="spanTable byte %d -- spans[0].fillValue -- goes from 0xff to 0x5a. "
               "Sixteen threads write the same byte, so the race is harmless."
               % (TABLE["spans"] + SPAN["fillValue"]),
        host="returned from all 16 threads; spanTable byte %d is 0x5a and nothing "
             "else in any buffer changed" % (TABLE["spans"] + SPAN["fillValue"]),
        risk="")


def k_connectspans():
    return dict(
        confidence=HIGH,
        loop="block 26 (back edge from 64); exits on `%67 = icmp eq i16 %66, -1` or "
             "`%68 = icmp eq i16 %65, -1`",
        reads="both are spans[id].nextSpanID -- offset +%d of the %d-byte span at "
              "spanTable+%d+%d*id, buffer 0"
              % (SPAN["nextSpanID"], SPAN_STRIDE, TABLE["spans"], SPAN_STRIDE),
        bufs=[span_table(0), first_span_in_rows(1)],
        does="id 0 returns before the loop (%11 is the literal -1 there); ids 1..3 run "
             "it once and leave on spans[id-1].nextSpanID == -1.",
        expect="NOTHING changes, and that is deliberate. Two rows' spans are disjoint, "
               "so `%%41 = icmp sgt max(startX), min(endX)` is true and the branch at 42 "
               "is skipped -- with it the `air.atomic.global.add` at 47 that appends an "
               "association at byte %d, 900 KiB past a 4096-byte buffer. The evidence "
               "for this one is the fence, not the buffer." % TABLE["associations"],
        host="returned from all 16 threads with not one byte changed, as above",
        risk="docs/air.md section 5 records this kernel as NOT finishing with air_run "
             "--zero, three boots out of three. On this machine the module returns on "
             "zeros at once, and it must: rowCount is then 0, `%14 = icmp ugt i32 "
             "%13, %2` is false, so the walk is handed the literal -1 and leaves "
             "before the loop. Those two cannot both be right. The candidates are our "
             "translation of this kernel (which this check does not cover -- it runs "
             "Apple's LLVM) and the log's fill label, which has already been wrong once "
             "for this exact kernel. One dispatch of `air_run --zero` alone in a fresh "
             "boot settles it, and air_run now prints the fill it used. "
             "The overlapping variant, which would write spans[id].parentSpanID and be "
             "visible, is NOT offered: four threads share each id, and whether they all "
             "load parentSpanID before one of them stores it is a question about "
             "lockstep execution rather than about this input. A wrong answer there "
             "takes the association branch and the write goes 900 KiB out of bounds.")


def k_draweye():
    quads = Blob(2, "eyeQuads", "constant", "MetalFaceMaskEyeQuads_t")
    quads.put(0, "I", 0,
              "nQuads = 0: `%33 = icmp eq i32 nQuads, 0` sends block 67 straight past "
              "the quad loop at 80, whose only exit is `%207 = icmp eq %206, nQuads`. A "
              "non-zero value must be <= 16 -- quads[] holds 16 in a 528-byte buffer, "
              "and the pattern's value walks off both")
    quads.put(4, "I", 1,
              "featherDistance = 1: a divisor (`%73 = %71 / float(featherDistance)`), "
              "so 0 gives an inf rather than a hang, but 1 is the honest value")
    quads.put(8, "B", 0x80,
              "fillValue = 128: mixed in with weight 0 when nQuads is 0, so it does not "
              "reach the texture; written for completeness")
    return dict(
        confidence=HIGH,
        loop="two. The span chain at 39 (back edge from 234) exits on `%236 = icmp eq "
             "i16 %235, -1`; the quad loop at 80 exits on `%207 = icmp eq i32 %206, "
             "%32`, where %32 is eyeQuads.nQuads",
        reads="%%235 is spans[id].nextSpanID (offset +%d of the span, buffer 3); %%32 is "
              "nQuads at offset 0 of buffer 2" % SPAN["nextSpanID"],
        bufs=[int4(0, "eyeBounds", (0, 0, 63, 63),
                   ("not read by this kernel",
                    "the row is `eyeBounds.y + tid - firstRowPos`",
                    "not read", "not read")),
              int4(1, "region", (0, 0, 64, 64),
                   ("the write is clipped to `all(p >= region.xy)` and offset by it",
                    "likewise", "and to `all(p < region.zw)`", "likewise")),
              quads, span_table(3), first_span_in_rows(4)],
        does="row = eyeBounds.y + tid - firstRowPos = tid, which `icmp slt rowCount` "
             "accepts and `sext` then indexes -- so eyeBounds.y - firstRowPos must not "
             "be negative, and 0 - 0 is not. One span per row, four pixels, then -1.",
        expect="four texels per row, each the span's fillValue/255 = 1.0h (the feather "
               "is 0: with no quads the distance stays FLT_MAX). The harness does not "
               "read textures back, so what is measurable is that the GPU finished.",
        host="returned from all 16 threads; 64 texels written (4 rows x 4 pixels x the 4 "
             "threads that share each row), the first at (0,0) with value 1",
        risk="It declares `texture2d<half, write>`, and metal_drv_test's own probe found "
             "no format this hardware gives as a texture a shader writes, so the "
             "pipeline is likely refused by name before these buffers matter. That "
             "refusal is a texture answer and not an input answer. air_run makes images "
             "with the format the kernel's intrinsics declare and is the harness for "
             "this one -- see the note on air_run's --buffer at the end.")


def k_drawspans():
    return dict(
        confidence=HIGH,
        loop="block 24 (back edge from 81); exits on `%83 = icmp eq i16 %82, -1`. The "
             "run over x is in the same loop and is bounded by endX, a 16-bit field, so "
             "it cannot run away whatever is in it",
        reads="%%82 is spans[id].nextSpanID -- offset +%d of the span, buffer 1"
              % SPAN["nextSpanID"],
        bufs=[int4(0, "region", (0, 0, 64, 64),
                   ("the write is clipped to `all(p >= region.xy)` and offset by it",
                    "likewise", "and to `all(p < region.zw)`", "likewise")),
              span_table(1), first_span_in_rows(2)],
        does="row tid draws spans[tid]'s four pixels at y = firstRowPos + tid and then "
             "leaves on -1. The furthest texel is x = 33, inside the harness's 64x64.",
        expect="four texels per row written with fillValue/255 = 1.0h; no device buffer "
               "changes, so again the evidence is the fence.",
        host="returned from all 16 threads; 64 texels written, the first at (0,0) with "
             "value 1",
        risk="Same writable-texture question as DrawEye.")


def k_generatemask():
    return dict(
        confidence=MED,
        loop="block 33 (back edge from 88); exits on `%95 = icmp slt i32 %39, %16` -- x "
             "against bounds.z",
        reads="%%16 is bounds.z, byte 8 of the 16-byte constant buffer 0. The pattern "
              "puts %d there and %d in bounds.x: a scan of %d pixels per thread with a "
              "texture read each time round"
              % (pattern_word(8), pattern_word(0), pattern_word(8) - pattern_word(0)),
        bufs=[int4(0, "bounds", (0, 0, 3, 3),
                   ("the scan starts at bounds.x", "the row is bounds.y + tid",
                    "THE BOUND: the scan ends at bounds.z, so this is 4 pixels",
                    "a thread with bounds.y + tid > bounds.w returns at once")),
              int4(1, "region", (0, 0, 64, 64),
                   ("the texture is read at (x, y) - region.xy", "likewise",
                    "not read", "not read")),
              Blob(2, "cubeInputScaling", "constant",
                   "MetalFaceMaskCubeInputScaling_t"),
              span_table(3, span_count=0, spans=False),
              Blob(4, "firstSpanInRows", "device", "ushort")],
        does="a producer, not a consumer: it scans four pixels of its row, classifies "
             "each through the 3D LUT and appends spans with an atomic add on "
             "spanCount. Starting spanCount at 0 keeps those appends in bounds: at most "
             "one per pixel per thread is 4*16 = 64 spans, and %d fit."
             % SPANS_THAT_FIT,
        expect="spanCount at most 64 and in practice 16, that many spans, "
               "firstSpanInRows[tid] set, and rowCount = 1 - bounds.y + bounds.w = 4 "
               "with firstRowPos = bounds.y = 0 written at the end (both already hold, "
               "so neither byte changes). The table it leaves is NOT a valid table -- "
               "four threads race for each row -- so it must not be fed to a consumer: "
               "the consumers above get the table this tool builds.",
        texflags=TEXFMT_CUBE,
        host="returned from all 16 threads; spanCount came back 16 -- one span per thread, "
             "well inside the %d that fit -- and firstSpanInRows[0..3] = 12, 13, 14, "
             "15, which are the four racing threads' last appends" % SPANS_THAT_FIT,
        risk="cubeInputScaling is left zero, and that is a choice rather than a "
             "derivation: a scale of 0 maps every pixel to LUT (0,0,0), which is in "
             "range, so the sample happens and the loop bound is untouched. It changes "
             "how many spans are appended, not whether the kernel ends. It also wants "
             "texture 0 as `texture3d<half, sample>` and texture 1 as "
             "`texture2d<float, read>`, hence the --texslots above; whether this "
             "hardware gives a sampleable 3D RGBA16Float is the harness's question to "
             "answer, and a refusal there is not a hang.")


def k_generatetoothmask():
    return dict(
        confidence=HIGH,
        loop="block 28 (back edge from 71); exits on `%78 = icmp slt i32 %33, %15` -- x "
             "against bounds.z",
        reads="%%15 is bounds.z, byte 8 of the 16-byte constant buffer 0 -- the pattern's "
              "value there is the %d-pixel scan this kernel was measured hanging on "
              "(logs/190426-run.log, position 4 of a fresh boot)"
              % (pattern_word(8) - pattern_word(0)),
        bufs=[int4(0, "bounds", (0, 0, 3, 3),
                   ("the scan starts at bounds.x", "the row is bounds.y + tid",
                    "THE BOUND: the scan ends at bounds.z, so this is 4 pixels",
                    "a thread with bounds.y + tid > bounds.w returns at once")),
              int4(1, "region", (0, 0, 64, 64),
                   ("the texture is read at (x, y) - region.xy", "likewise",
                    "not read", "not read")),
              Blob(2, "cubeInputScaling", "constant",
                   "MetalFaceMaskCubeInputScaling_t"),
              span_table(3, span_count=0, spans=False),
              Blob(4, "firstSpanInRows", "device", "ushort")],
        does="the same producer shape as GenerateMask, classifying on a dot product of "
             "the RGB texel instead of a LUT -- so it needs no 3D texture, and the "
             "read-only RGBA32Float 2D the harness falls back to is what it declares.",
        expect="firstSpanInRows[tid] written, and at most 64 appended spans -- how "
               "many depends on the texels and not on this input, and with the ones "
               "this harness writes it is none at all (see `host`). rowCount = 4 and "
               "firstRowPos = 0 are written and already hold. cubeInputScaling is "
               "`air.arg_unused` here, which is Apple's own metadata saying the zeros "
               "do not matter.",
        texflags=TEXFMT_2D,
        host="returned from all 16 threads; it appended NOTHING and wrote "
             "firstSpanInRows[0..3] = -1. With the texel formula this harness uses "
             "-- (100+x, 200+y, 300+x+y, 400) -- the classifier's dot product comes "
             "out at about -2.35 and its test is `> -0.008`, so no pixel is inside. "
             "That is a statement about the texels, not about the bound: the loop "
             "ran its four pixels per thread either way",
        risk="")


def k_propagateparents():
    cfg = Blob(0, "config", "constant", "MetalFaceMaskPropogateConfig_t")
    cfg.put(0, "I", 0, "offset = 0: the row is `offset + tid`")
    cfg.put(4, "I", GRID,
            "minorCount = %d: `%%7 = udiv i32 tid, minorCount`, so it MUST NOT BE 0 -- "
            "the all-zero buffers the test writes today divide by zero here" % GRID)
    cfg.put(8, "I", GRID,
            "majorStride = %d, equal to minorCount, so `(majorStride - minorCount) * "
            "(tid / minorCount)` is 0 and the row index is just tid" % GRID)
    return dict(
        confidence=HIGH,
        loop="block 26 (self loop); exits on `%36 = icmp eq i16 %35, -1`",
        reads="%%35 is spans[id].nextSpanID -- offset +%d of the span, buffer 1"
              % SPAN["nextSpanID"],
        bufs=[cfg, span_table(1), first_span_in_rows(2)],
        does="row = offset + tid = tid < rowCount, so each id walks its own one-span "
             "chain once and leaves on -1.",
        expect="spans[tid].parentSpanID is stored with "
               "spans[spans[tid].parentSpanID].parentSpanID, which for parentSpanID = "
               "tid is the value already there. So the buffer comes back as it went in "
               "and the store is idempotent -- which is what makes it safe for the four "
               "threads that share each id.",
        host="returned from all 16 threads with not one byte changed, which is what an "
             "idempotent store looks like",
        risk="")


def k_resolveassociations():
    return dict(
        confidence=EMPTY,
        loop="block 6 over the associations, exiting on `%66 = icmp eq i32 %65, %3` "
             "(the associationCount), and an INNER walk at 57 that leaves only on `%63 "
             "= icmp eq i16 %62, %45` -- a ring of nextRingSiblingID coming back round "
             "to where it started",
        reads="%%3 is associationCount, byte %d of buffer 0; %%62 is "
              "spans[id].nextRingSiblingID, offset +%d of the span"
              % (TABLE["associationCount"], SPAN["nextRingSiblingID"]),
        bufs=[span_table(0, span_count=0, spans=False)],
        does="with associationCount = 0 it returns before the loop -- which is what the "
             "all-zero buffers the test writes today already do. So this kernel does "
             "not hang the harness; it also does nothing, and a dispatch measures only "
             "that.",
        expect="nothing: the kernel returns at block 5.",
        host="returned from all 16 threads having touched nothing, as it must with "
             "associationCount = 0",
        risk="IT CANNOT BE EXERCISED HERE AT ALL, and that is a statement about the "
             "harness rather than the kernel. associations[] begins at byte %d of a "
             "buffer bound at %d, so the first iteration of the outer loop reads 900 "
             "KiB past the end and the inner ring walk writes there. If a harness ever "
             "binds the declared 1179628 bytes, the safe input is reasoned to be "
             "associationCount = 1, associations[0] = {parentSpanID 0, childSpanID 1}, "
             "spans[0].parentSpanID = 0, spans[1].parentSpanID = 1 and BOTH "
             "nextRingSiblingID = -1: `%%25` and `%%37` are then true, which makes `%%49 "
             "= icmp eq %%42, %%45` true and skips the ring walk at 57 entirely, and the "
             "outer loop ends after one association. That is reasoned and not measured, "
             "and it is deliberately not in the hex below."
             % (TABLE["associations"], HARNESS_BYTES))


def k_bistochastize_iter():
    return dict(
        confidence=HIGH,
        loop="block 42 (self loop); exits on `%53 = icmp eq i32 %52, %21`, a counter "
             "that starts at grid_buf[tid-1].blur_offset and increments by one",
        reads="%%21 is grid_buf[tid].blur_offset, byte +20 of the 24-byte HashEntry in "
              "buffer 0, and %%19 is the previous entry's. The loop is guarded by `%%30 = "
              "icmp ult %%19, %%21`, so the count is exactly their difference -- which "
              "the pattern makes %d for thread 0, whose counter starts at the literal 0"
              % pattern_word(20),
        bufs=[fbs_grid(0), fbs_bidx(1),
              fbs_floats(2, "Dn_in_buf", 1.0, "a weight, read at [tid] and at [bidx]"),
              Blob(3, "Dn_out_buf", "device", "float"), fbs_params(4)],
        does="N = 4 lets ids 0..3 through; each then counts from k to k+1 -- one "
             "iteration, reading bidx_buf[k] and Dn_in_buf[bidx_buf[k]].",
        expect="Dn_out_buf[0..3] = sqrt(count * Dn_in / (Dn_in * 2 * dims + "
               "Dn_in[bidx])) = sqrt(1/3) = 0x%08x, up to whatever air.fast_sqrt does "
               "in the last bit. Four threads per id write the same word."
               % f32(0.5773502691896257),
        host="returned from all 16 threads; Dn_out_buf[0..3] came back 3a cd 13 3f each, "
             "which is the 0x3f13cd3a predicted above",
        risk="")


def k_pcg_init():
    return dict(
        confidence=MED,
        loop="two. The walk at 46 (self loop) exits on `%61 = icmp eq i32 %60, %33`; "
             "the reduction at 87 exits on `%95 = icmp eq i16 %94, 128`, a constant, "
             "and is no hazard",
        reads="%33 is grid_buf[gid].blur_offset, byte +20 of the HashEntry in buffer 0",
        bufs=[fbs_grid(0), fbs_bidx(1),
              fbs_floats(2, "Dn_in_buf", 1.0, "a weight, read at [gid] and at [bidx]"),
              fbs_floats(3, "A_in_buf", 4.0,
                         "the diagonal; `%64 = fmax(A_in[gid], 1e-5)` is a divisor, and "
                         "4 keeps the result away from 0 so the write is visible"),
              fbs_floats(4, "b_in_buf", 1.0, "the right-hand side"),
              fbs_floats(5, "x_in_buf", 1.0, "the current solution"),
              Blob(6, "r_out_buf", "device", "float"),
              Blob(7, "d_out_buf", "device", "float"),
              Blob(8, "lnew_out_buf", "device", "metal::_atomic"),
              Blob(9, "has_converged", "device", "bool"),
              fbs_params(10)],
        does="one iteration per id, as bistochastize_iter; then a threadgroup reduction "
             "over 128 slots and one atomic add. Thread 0 first zeroes lnew_out_buf and "
             "has_converged, so those two need no input.",
        expect="r_out_buf[0..3] = b/2 - (A/2*x - lambda/2*Dn*Dn*x) = 0.5 - 1.5 = -1 "
               "(0x%08x) and d_out_buf[0..3] = -1 * 1/A = -0.25 (0x%08x)."
               % (f32(-1.0), f32(-0.25)),
        host="returned from all 16 threads; r_out_buf[0..3] = 0xbf800000 and "
             "d_out_buf[0..3] = 0xbe800000, the -1 and -0.25 predicted above",
        risk="lnew_out_buf is the one word here that the dispatch decides and not this "
             "input. The reduction sums all 128 entries of the threadgroup array, and a "
             "4x4x1 group indexes that array with the x component of tid: 4 entries are "
             "written, 124 are whatever was left in threadgroup memory, and four threads "
             "have tid == 0, so four of them add the same sum. Two boots of these bytes "
             "gave 0x00400000 and 0xe2180000. It is not a hang; r_out and d_out are "
             "per-thread and unaffected. A 128x1x1 group, which is what the array and the "
             "literal 128 are written for, would make it an equality -- docs/air.md "
             "section 5.")


def k_pcg_iter1():
    return dict(
        confidence=MED,
        loop="two, as pcg_init: the walk at 47 exits on `%62 = icmp eq i32 %61, %34`, "
             "the reduction at 80 on the constant 128",
        reads="%34 is grid_buf[gid].blur_offset, byte +20 of the HashEntry in buffer 0",
        bufs=[fbs_grid(0), fbs_bidx(1),
              fbs_floats(2, "Dn_in_buf", 1.0, "a weight, read at [gid] and at [bidx]"),
              fbs_floats(3, "A_in_buf", 4.0, "the diagonal"),
              fbs_floats(4, "d_in_buf", 1.0, "the search direction"),
              Blob(5, "q_out_buf", "device", "float"),
              Blob(6, "alpha_out_buf", "device", "metal::_atomic"),
              Blob(7, "has_converged", "constant", "bool"),
              fbs_params(8)],
        does="buffer 7 is read FIRST: `%12 = load i8` and `%13 = icmp eq i8 %12, 0` "
             "send a non-zero has_converged straight to the return. It stays 0 here, "
             "which is the input that reaches the loop at all -- with 0xff this kernel "
             "always finishes, and finishing then proves nothing.",
        expect="q_out_buf[0..3] = A/2*d - lambda/2*Dn*Dn*d = 2 - 0.5 = 1.5 (0x%08x)."
               % f32(1.5),
        host="returned from all 16 threads; q_out_buf[0..3] = 0x3fc00000, the 1.5 predicted "
             "above",
        risk="alpha_out_buf is the dispatch's for the same 128-entry reason as pcg_init's "
             "lnew_out_buf, and varied the same way: 0x00c00000 in one boot and "
             "0xe3800000 in another.")


KERNELS = [
    # the eight MetalFaceMask_* of kstruct's endless group
    ("MetalFaceMask_AddSeedPoint", k_addseedpoint),
    ("MetalFaceMask_ConnectSpans", k_connectspans),
    ("MetalFaceMask_DrawEye", k_draweye),
    ("MetalFaceMask_DrawSpans", k_drawspans),
    ("MetalFaceMask_GenerateMask", k_generatemask),
    ("MetalFaceMask_GenerateToothMask", k_generatetoothmask),
    ("MetalFaceMask_PropagateParents", k_propagateparents),
    ("MetalFaceMask_ResolveAssociations", k_resolveassociations),
    # and the three fbs_* of that group which bind only buffers. Those three plus
    # AddSeedPoint, ConnectSpans, PropagateParents and ResolveAssociations are
    # exactly the seven rc.jobs.metalcompute leaves out of its sixteen; the four
    # other MetalFaceMask_* above are out of that list because they bind a
    # texture, not because they loop.
    ("fbs_bistochastize_iter", k_bistochastize_iter),
    ("fbs_pcg_init", k_pcg_init),
    ("fbs_pcg_iter1", k_pcg_iter1),
]

# The rest of kstruct's fourteen, and why they are not here. Each row is a
# reading of the module rather than a shrug: the loop and the value it leaves on
# are named, and so is what is missing.
UNFINISHED = [
    ("fbs_bistochastize_final",
     "blocks 52 and 79; `%63 = icmp eq i32 %62, %28` and `%103 = icmp eq i32 %102, %45`",
     "%28 is grid_buf[].blur_offset, the same prefix sum fbs_grid() builds; %45 is "
     "coordidx_buf[k+1], a SECOND row-pointer array, in buffer 2",
     "the first loop takes the recipe above unchanged. The second needs coordidx_buf "
     "and coordtbl_buf, whose entries index a coordinate table whose shape this reading "
     "did not establish, and it also reads two textures. Not guessed."),
    ("fbs_slice_trilinear",
     "blocks 120, 170, 220 and 267, each an `icmp eq` against a value loaded from "
     "buffer 0",
     "interp_idx_buf[k] .. interp_idx_buf[k+1]: four separate rows of one more prefix "
     "sum, read through interp_tbl_buf and interp_pad_buf",
     "four walks of the same shape, so the recipe is PROBABLY interp_idx_buf[k] = k -- "
     "but four loops over three tables and a written texture is more than one reading "
     "settles, and probably is not a standard worth a boot."),
    ("confidence",
     "blocks 68, 88, 133, 151, 167 and 194",
     "four of the six leave on the constants 64, 64, 128 and 131. The other two leave "
     "on `%148 = select(%143 ult 127, %143, 6)`, derived from a loop counter, and on "
     "`%180 = max(0, min(128, ...))`",
     "THIS ONE IS A FALSE POSITIVE of kstruct's signal, which is the finding rather "
     "than a gap. %180 does trace back to a load, which is what the signal looks for, "
     "but it is clamped to [0, 128] before the loop uses it and the counter starts at "
     "0; %148 and its starting value both come from a counter bounded by 131. So every "
     "loop in `confidence` is bounded by a constant whatever the buffers hold, and it "
     "needs no input at all. Whether its three textures and two samplers can be bound "
     "is a separate question."),
]


# ---------------------------------------------------------------- the checks

def verify(kernels):
    """Every offset here against Apple's metadata, every buffer against the name
    and address space it declares, and every kernel against kstruct. Refused
    rather than warned: hex nobody checked is worse than no hex.
    """
    bad = []
    names = []
    q = subprocess.run([AIR2NIR, "--list", os.path.join(ROOT, LIB)], capture_output=True)
    for line in q.stdout.decode(errors="replace").splitlines():
        f = line.split()
        if len(f) >= 2 and f[1] == "kernel":
            names.append(f[0])
    seen = {}
    for name, build in kernels:
        if name not in names:
            bad.append("%s is not a kernel of %s" % (name, LIB))
            continue
        ll = module(name)
        _nloops, exit_loaded, _ind = kstruct.loops_and_signals(ll)
        if not exit_loaded:
            bad.append("%s: kstruct no longer calls this one endless, so either the "
                       "frontend changed or this file is stale" % name)
        args, nodes = arg_table(ll)
        mine = dict((b.slot, b) for b in build()["bufs"])
        for a in args:
            if a["kind"] != "buffer":
                continue
            b = mine.get(a["slot"])
            if b is None:
                bad.append("%s: buffer %d (%s) is declared and not built here"
                           % (name, a["slot"], a.get("arg_name")))
                continue
            if b.name != a.get("arg_name"):
                bad.append("%s: buffer %d is %s here and %s in the metadata"
                           % (name, a["slot"], b.name, a.get("arg_name")))
            want = "constant" if a.get("address_space") == 2 else "device"
            if b.kind != want:
                bad.append("%s: buffer %d is %s here and %s in the metadata"
                           % (name, a["slot"], b.kind, want))
            if b.type_name != a.get("arg_type_name"):
                bad.append("%s: buffer %d is a %s here and a %s in the metadata"
                           % (name, a["slot"], b.type_name, a.get("arg_type_name")))
            if a["struct"] is not None:
                collect_structs(nodes, a["struct"], a.get("arg_type_name"), seen)
        for slot in sorted(mine):
            if not any(a["kind"] == "buffer" and a["slot"] == slot for a in args):
                bad.append("%s: buffer %d is built here and not declared" % (name, slot))
    for tname, want in sorted(LAYOUT.items()):
        got = seen.get(tname)
        if got is None:
            bad.append("%s: no argument of the kernels checked carries this struct, so "
                       "its offsets here went unchecked" % tname)
            continue
        gotd = dict((f, (o, s)) for f, o, s in got)
        for field, off, size in want:
            if field not in gotd:
                bad.append("%s has no field %s in the metadata" % (tname, field))
            elif gotd[field] != (off, size):
                bad.append("%s.%s is at %d size %d here and at %d size %d in the "
                           "metadata" % ((tname, field, off, size) + gotd[field]))
    return bad


# ---------------------------------------------------------------- the output

def wrap(text, indent, width=92):
    lines, cur = [], ""
    for w in text.split():
        if cur and len(cur) + len(w) + 1 > width - indent:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    lines.append(cur)
    return ("\n" + " " * indent).join(lines)


def command(name, k):
    args = sum([b.args() for b in k["bufs"]], [])
    flags = ([k["texflags"]] if k.get("texflags") else []) + args
    return "/redwood/bin/metal_drv_test %s %s %s" % (" ".join(flags), ON_DEVICE, name)


def report(kernels, hexonly, jobs):
    if jobs:
        print("# Kernels kstruct.py calls endless, with the buffer contents that let "
              "them end")
        print("# (tools/air/kinput.py --jobs). Read the table -- kinput.py with no "
              "arguments --")
        print("# before running a line: it says what each number is and how sure it is.")
        print("# --buffer N@OFF=HEX is not a flag metal_drv_test has yet.")
    elif not hexonly:
        print("kinput: the input that lets %d of kstruct's endless kernels end, built "
              "for a %d-byte buffer and a %dx%d group"
              % (len(kernels), HARNESS_BYTES, GRID, GRID))
        print("        %s" % LIB)
        print()
    for name, build in kernels:
        k = build()
        if jobs:
            if k["confidence"] == NONE:
                print("# %s: no input established, DO NOT RUN" % name)
            elif k["confidence"] == EMPTY:
                print("%s   # %s: safe, but the loop is never entered"
                      % (command(name, k), k["confidence"]))
            else:
                print("%s   # %s" % (command(name, k), k["confidence"]))
            continue
        if hexonly:
            print("%s   (%s)" % (name, k["confidence"]))
            if k["confidence"] == NONE:
                print("    no input established; the table says why")
            for b in k["bufs"]:
                for a in b.args():
                    print("    %s" % a)
            if k.get("texflags"):
                print("    %s" % k["texflags"])
            print()
            continue
        print("=" * 92)
        print("%-62s confidence: %s" % (name, k["confidence"]))
        print("  loop    %s" % wrap(k["loop"], 10))
        print("  reads   %s" % wrap(k["reads"], 10))
        for b in k["bufs"]:
            print("  buffer %-2d %-18s %s %s%s"
                  % (b.slot, b.name, b.kind, b.type_name,
                     "" if b.rows else "   -- left as the zeros the harness writes"))
            for off, fmt, shown, why in b.rows:
                print("    %6d  %-4s %-10s %s" % (off, LABEL[fmt], shown, wrap(why, 28)))
        if k["does"]:
            print("  then    %s" % wrap(k["does"], 10))
        if k["expect"]:
            print("  expect  %s" % wrap(k["expect"], 10))
        if k.get("host"):
            print("  host    %s" % wrap(k["host"], 10))
        if k.get("texflags"):
            print("  texture %s" % k["texflags"])
        if k["risk"]:
            print("  careful %s" % wrap(k["risk"], 10))
        print()


# ---------------------------------------------------------------------------
# --check LOG: what the device did, against what the CPU did
#
# The bytes above are an input. These are the output the same input produced on
# the CPU, where the module runs as Apple's own LLVM IR -- air2nir --ll printed
# it, the air64 triple and the address spaces were dropped, the air.* calls got
# host bodies, and a driver bound sixteen zeroed 4096-byte buffers and ran a
# 4x4 grid one thread at a time. All eleven returned there.
#
# So this table is a SECOND REFERENCE and not a specification: it says what
# Apple's own instructions compute from these bytes, with our translation and
# the hardware both out of the picture. A device run that disagrees with it is
# therefore a statement about our translation or about the Adreno -- which is
# the whole point of writing it down -- and a device run that agrees proves
# both only for the words named here.
#
# Each entry is a list of (slot, byte offset, hex) that MUST differ afterwards
# and hold exactly those bytes, plus `partial` when the kernel also changes
# words this table does not predict, and `texels` for the two that write a
# texture instead of a buffer. An empty list means NOTHING may change, which
# for three of these kernels is the measurement (the association branch they
# must not take writes 900 KiB out of bounds).
#
# Reproducing the reference is a shell script's worth of work and is written
# down in docs/air.md section 5; it is deliberately not a repo tool, because it
# carries Apple's modules as text.
AFTER = {
    "MetalFaceMask_AddSeedPoint":      dict(bufs=[(3, 28, "5a")]),
    "MetalFaceMask_ConnectSpans":      dict(bufs=[]),
    "MetalFaceMask_PropagateParents":  dict(bufs=[]),
    "MetalFaceMask_ResolveAssociations": dict(bufs=[], weak="its loop is never entered, so "
                                             "agreeing here is writing nothing"),
    "MetalFaceMask_DrawSpans":         dict(bufs=[], texels=(64, 1.0)),
    "MetalFaceMask_DrawEye":           dict(bufs=[], texels=(64, 1.0)),
    "MetalFaceMask_GenerateToothMask": dict(bufs=[(4, 0, "ffffffffffffffff")]),
    # Sixteen threads claim span indices through one atomic counter, so what lands in
    # buffer 4 depends on the order they get them in: the CPU's 0c 00 0d 00 0e 00 0f 00
    # is one legal outcome and not the only one. It is therefore not required here.
    "MetalFaceMask_GenerateMask":      dict(bufs=[(3, 8, "10")], partial=True,
                                            weak="the span indices come from an atomic "
                                            "counter, so buffer 4 is order-dependent and no "
                                            "byte of it can be required"),
    "fbs_bistochastize_iter":          dict(bufs=[(3, 0, "3acd133f" * 4)]),
    # The reduction word is deliberately NOT here. Both kernels end with
    # `if (tid == 0)` summing all 128 entries of a threadgroup array and one
    # atomic_fetch_add, and `tid` is Metal's scalar thread_position_in_threadgroup
    # -- the x component, not the flattened index. Under air_run's 4x4x1 group
    # four threads have tid == 0, so four of them run the whole reduction and
    # each adds its own copy; only x = 0..3 ever index the array, so 4 of the 128
    # entries are written and 124 are whatever was left in threadgroup memory.
    # The word is therefore 4 x (the 4 entries + that leftover), which is a
    # property of the dispatch. It is not a reference the CPU can state either:
    # the simulator runs the sixteen threads one after another and every thread
    # with gid == 0 re-zeroes the counter before adding, so only the last of the
    # four adds survives -- walk the same sixteen threads with x in the outer
    # loop instead and the same bytes give 0x00040000 instead of 0x00100000.
    # docs/air.md section 5 has the four measurements this is read off.
    "fbs_pcg_init":                    dict(bufs=[(6, 2, "80bf"), (6, 6, "80bf"),
                                                  (6, 10, "80bf"), (6, 14, "80bf"),
                                                  (7, 2, "80be"), (7, 6, "80be"),
                                                  (7, 10, "80be"), (7, 14, "80be")],
                                            partial=True,
                                            weak="lnew_out_buf word 0 cannot be an equality "
                                            "under this dispatch: four threads of the 4x4x1 "
                                            "group have tid == 0 and each adds a sum over "
                                            "128 threadgroup entries of which the dispatch "
                                            "writes 4. 0x00400000 is 4 x (4 x 0x00040000), "
                                            "and the CPU's 0x00100000 is one add of the same "
                                            "sum because its threads ran one at a time. A "
                                            "128x1x1 group makes it the equality 0x00100000"),
    "fbs_pcg_iter1":                   dict(bufs=[(5, 2, "c03f"), (5, 6, "c03f"),
                                                  (5, 10, "c03f"), (5, 14, "c03f")],
                                            partial=True,
                                            weak="alpha_out_buf word 0 cannot be an equality "
                                            "for the same reason as fbs_pcg_init's "
                                            "lnew_out_buf: 0x00c00000 in one boot is 4 x the "
                                            "sum of its 4 written entries, and 0xe3800000 in "
                                            "another is 4 x (that sum + 0x38800000 left in "
                                            "an entry nobody wrote)"),
}

# The canary of userland/rc.jobs.kinput, and what a healthy boot's dump of it
# looks like: six words and no others, measured on softpipe and on the device.
# After one hung dispatch this port's GPU stops making writes visible without
# saying so, and the canary is the only thing in a log that distinguishes "the
# kernel wrote nothing" from "nothing could write any more".
CANARY = "MetalFaceMask_InitMinMax"
CANARY_WORDS = 6


class Run(object):
    """One air_run job as a boot log records it."""

    def __init__(self, kernel, line):
        self.kernel = kernel
        self.line = line
        self.finished = True
        self.changed = {}      # slot -> {word: (before, after)}
        self.texels = None
        self.patched = {}      # slot -> bytes the --buffer flags put there
        self.fill = None
        self.cut = False       # the loader dropped the log under this run


JOB_RE = re.compile(r"^air_run: (\S+) on .*, as a compute kernel")
PATCH_RE = re.compile(r"^air_run: buffer at index (\d+), (\d+) of its (\d+) bytes from --buffer")
FILL_RE = re.compile(r"^air_run: dispatching .* filled with (.*)$")
BUF_RE = re.compile(r"^air_run: device buffer (\d+) after the dispatch")
WORD_RE = re.compile(r"^  changed \[\s*(\d+)\] 0x([0-9a-f]{8}) -> 0x([0-9a-f]{8})")
TEX_RE = re.compile(r"^air_run: image (\d+) after the dispatch")
TEXEL_RE = re.compile(r"^\s*\((\d+),\s*(\d+)(?:,\s*\d+)?\)\s+(\S+)")


CUT_RE = re.compile(r"the log between the first .* was dropped")


def read_log(path):
    """Every air_run compute job in a boot log, in the order it ran.

    The loader keeps the first 256 KB of a boot's log and the last 1 MB and
    drops what is between, leaving one marker where it did. A run the marker
    falls inside printed more than this log holds, and reading its silence as a
    result is how a harness limit gets reported as a difference -- so the run
    it interrupts is marked, and so is every kernel that is missing while a cut
    exists, because the missing one may be in the dropped middle.
    """
    runs, cur, slot, cut = [], None, None, False
    for line in open(path, errors="replace"):
        line = line.replace("\0", "").rstrip("\n")
        if CUT_RE.search(line):
            cut = True
            if cur is not None:
                cur.cut = True
            continue
        m = JOB_RE.match(line)
        if m:
            cur = Run(m.group(1).split("/")[-1], line)
            runs.append(cur)
            slot = None
            continue
        if cur is None:
            continue
        m = FILL_RE.match(line)
        if m:
            cur.fill = m.group(1)
        m = PATCH_RE.match(line)
        if m:
            cur.patched[int(m.group(1))] = int(m.group(2))
        m = BUF_RE.match(line)
        if m:
            slot = int(m.group(1))
            cur.changed.setdefault(slot, {})
            continue
        m = WORD_RE.match(line)
        if m and slot is not None:
            cur.changed[slot][int(m.group(1))] = (int(m.group(2), 16), int(m.group(3), 16))
            continue
        if "the dispatch did not finish" in line or "THE GPU DID NOT FINISH" in line:
            cur.finished = False
        m = TEX_RE.match(line)
        if m:
            cur.texels = []
            continue
        if cur.texels is not None:
            m = TEXEL_RE.match(line)
            if m:
                cur.texels.append(m.group(3))
    return runs, cut


def want_bytes(entry):
    """The (slot, byte) -> value the CPU reference asks for.

    Byte and not word granularity, because the reference's spans are not all
    word-aligned: a `ushort out_index` into a float array leaves two bytes
    written and two untouched inside one word, and a word comparison would
    either refuse those rows or invent the two bytes it does not know.
    """
    want = {}
    for slot, off, hexs in entry.get("bufs", []):
        for i, byte in enumerate(bytes.fromhex(hexs)):
            want[(slot, off + i)] = byte
    return want


def check_run(run, entry):
    """AGREE / DIFFER for one job, with the reason in words.

    The log prints every changed word as `old -> new`, so the expected new word
    is the old one with the reference's bytes laid into it. That is what makes a
    byte-level expectation checkable against a word-level dump at all.
    """
    if not run.finished:
        return "DID NOT FINISH", ["the dispatch did not end, so nothing below it is evidence"]
    if run.cut:
        return "LOG CUT", ["the loader dropped the log inside this run, so what it printed "
                           "after its bindings is not here to compare"]

    want = want_bytes(entry)
    notes, bad = [], False
    covered = set()

    for slot in sorted(run.changed):
        for w in sorted(run.changed[slot]):
            before, after = run.changed[slot][w]
            expect, touched = before, False
            for k in range(4):
                key = (slot, 4 * w + k)
                if key in want:
                    expect = (expect & ~(0xff << (8 * k))) | (want[key] << (8 * k))
                    covered.add(key)
                    touched = True
            if after == expect:
                continue
            if touched:
                notes.append("buffer %d word %d is 0x%08x, the CPU's bytes make it 0x%08x"
                             % (slot, w, after, expect))
                bad = True
            elif entry.get("partial"):
                notes.append("buffer %d word %d changed to 0x%08x, which this reference does "
                             "not predict" % (slot, w, after))
            else:
                notes.append("buffer %d word %d changed to 0x%08x and the CPU left it alone"
                             % (slot, w, after))
                bad = True

    missed = sorted(k for k in want if k not in covered)
    if missed:
        for slot, off in missed[:8]:
            notes.append("buffer %d byte %d should have become 0x%02x, and the word holding "
                         "it did not change at all" % (slot, off, want[(slot, off)]))
        if len(missed) > 8:
            notes.append("and %d more bytes the CPU wrote and this run did not"
                         % (len(missed) - 8))
        bad = True

    if entry.get("texels"):
        n, value = entry["texels"]
        if run.texels is None:
            notes.append("no texture was read back, and the CPU wrote %d texels of %g"
                         % (n, value))
            bad = True
        else:
            notes.append("a texture came back with %d texels printed; the CPU wrote %d of %g "
                         "(air_run prints a corner, so this is not a count)"
                         % (len(run.texels), n, value))
    if entry.get("weak"):
        notes.append("and this row is weak evidence whatever it says: %s" % entry["weak"])

    if bad:
        return "DIFFER", notes
    if not want and not entry.get("texels"):
        return "AGREE (nothing changed, as the CPU)", notes or ["not one word differs"]
    if entry.get("partial"):
        return "AGREE so far", notes
    return "AGREE", notes or ["every byte the CPU wrote, and no word it did not"]


def check(path):
    runs, cut = read_log(path)
    if not runs:
        print("kinput: %s holds no air_run compute job at all" % path)
        return 1

    # A log from before air_run printed the words a kernel changed carries no
    # such line at all, and then every dump reads as "wrote nothing" -- including
    # the canary's, which would put the boundary at job 1 and call a whole good
    # run unmeasured. Refuse to read it rather than report that.
    if not any(r.changed and any(w for w in r.changed.values()) for r in runs):
        print("kinput: %s was written before air_run printed the words a dispatch "
              "changed, so there is nothing here to compare. Re-run the list with a "
              "current air_run." % path)
        return 1

    # the canary first: everything after its last healthy dump is unmeasured, and
    # saying so before the verdicts keeps a wedged boot from reading as a result
    canaries = [i for i, r in enumerate(runs) if r.kernel == CANARY]
    boundary = len(runs)
    for i in canaries:
        words = sum(len(w) for w in runs[i].changed.values())
        if not runs[i].finished or words != CANARY_WORDS:
            boundary = i
            break
    print("kinput --check %s" % path)
    print("  %d air_run jobs, %d of them the canary" % (len(runs), len(canaries)))
    # What a DIFFER may be read as, printed where the verdicts are and not only
    # in this file's header. The reference has been re-derived with the grid
    # air_run actually dispatches -- 4x4x1, a scalar thread_position_in_grid
    # being x, so the ids are 0..3 with four threads each -- after a first
    # version of it got thread_position_in_threadgroup wrong for the two pcg
    # kernels. So a difference here is no longer about the two references
    # disagreeing on what a thread is. What it can still be about is the
    # compiler: two bugs in ir3 answer for every difference in this table as of
    # logs/065057-run.log, and a run with tools/mesa/ir3-ssbo-subdword.patch
    # and ir3-shift-split.patch in is the one that says so.
    print("  the reference is the same grid air_run dispatches (4x4x1, scalar ids 0..3),")
    print("  so a difference here is not the two references disagreeing about threads. It")
    print("  can still be the compiler: see tools/mesa/ir3-ssbo-subdword.patch and")
    print("  ir3-shift-split.patch, and whether this log was made with them in.")
    if boundary < len(runs):
        print("  the canary stopped writing at job %d of %d (%s): NOTHING from there on "
              "is evidence about a kernel" % (boundary + 1, len(runs), runs[boundary].kernel))
    elif canaries:
        print("  the canary wrote its %d words every one of its %d turns, so the GPU was "
              "answering for the whole run" % (CANARY_WORDS, len(canaries)))

    ledger, verdicts = {}, {}
    print()
    for i, run in enumerate(runs):
        if run.kernel == CANARY:
            continue
        entry = AFTER.get(run.kernel)
        if entry is None:
            verdict, notes = "NOT IN THE REFERENCE", ["this kernel has no CPU run written down"]
        elif i > boundary:
            verdict, notes = "UNMEASURED", ["it ran after the canary stopped writing"]
        else:
            verdict, notes = check_run(run, entry)
        # A kernel run twice in one boot is one row, not two: the list runs
        # ConnectSpans with its input and again with zeros, and counting both
        # made the ledger add to twelve out of eleven. Two runs that disagree
        # are their own verdict, because then the kernel is not deterministic
        # here and neither number is a measurement.
        if run.kernel in verdicts and verdicts[run.kernel] != verdict:
            ledger[verdicts[run.kernel]] -= 1
            verdict = "TWICE, AND NOT THE SAME"
            notes.append("an earlier run of it in this log said %s"
                         % verdicts[run.kernel])
        elif run.kernel in verdicts:
            print("  %-34s %s (again, same verdict)" % (run.kernel, verdict))
            for n in notes:
                print("        %s" % wrap(n, 8))
            continue
        verdicts[run.kernel] = verdict
        ledger[verdict] = ledger.get(verdict, 0) + 1
        print("  %-34s %s" % (run.kernel, verdict))
        if run.fill and "ZEROS" not in run.fill and entry is not None:
            print("        the fill was %s -- these bytes were written for zeros"
                  % run.fill.split(" (")[0])
        for n in notes:
            print("        %s" % wrap(n, 8))

    print()
    missing = [n for n, _b in KERNELS if not any(r.kernel == n for r in runs)]
    print("  ledger, which has to add up to the %d kernels this file knows:" % len(KERNELS))
    for verdict in sorted(ledger):
        print("    %3d %s" % (ledger[verdict], verdict))
    if missing:
        print("    %3d not in this log at all%s: %s"
              % (len(missing),
                 ", and this log has a cut, so they may be in the dropped middle" if cut
                 else "", ", ".join(missing)))
    total = sum(ledger.values()) + len(missing)
    extra = [r.kernel for r in runs if r.kernel != CANARY and r.kernel not in dict(KERNELS)]
    if extra:
        print("    (%d jobs in the log are not of this group: %s)"
              % (len(extra), ", ".join(sorted(set(extra)))))
    print("    %3d total" % total)
    if total != len(KERNELS):
        print("  kinput: the ledger does NOT add up, so read it as broken, not as a result")
        return 1
    return 0


def main(argv):
    hexonly = "--hex" in argv
    jobs = "--jobs" in argv
    if "--check" in argv:
        logs = [a for a in argv[argv.index("--check") + 1:] if not a.startswith("--")]
        if not logs:
            print("usage: kinput.py --check LOG...", file=sys.stderr)
            return 2
        return max(check(l) for l in logs)
    want = [a for a in argv if not a.startswith("--")]
    if not os.path.exists(AIR2NIR):
        print("kinput: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1
    if not os.path.exists(os.path.join(ROOT, LIB)):
        print("kinput: missing %s" % LIB, file=sys.stderr)
        return 1

    known = dict(KERNELS)
    if want and any(w not in known for w in want):
        print("kinput: %s is not one of the kernels here"
              % ", ".join(w for w in want if w not in known), file=sys.stderr)
        return 1
    kernels = [(n, b) for n, b in KERNELS if not want or n in want]

    # the whole file is checked whatever is asked for: the offsets are a property
    # of this file and not of the selection, and a check that a filter can switch
    # off is a check nobody runs.
    bad = verify(KERNELS)
    if bad:
        for line in bad:
            print("kinput: %s" % line, file=sys.stderr)
        print("kinput: the offsets here and the library's own metadata disagree, so "
              "none of the bytes can be trusted. Nothing printed.", file=sys.stderr)
        return 1

    report(kernels, hexonly, jobs)
    if hexonly or jobs or want:
        return 0        # the closing count is about the whole group, not a selection

    print("=" * 92)
    print("kinput: %d kernels above, every one of them in kstruct's group of 14. The "
          "other three, and why they are not:" % len(kernels))
    for name, loop, reads, why in UNFINISHED:
        print("  %s" % name)
        print("    loop   %s" % wrap(loop, 11))
        print("    reads  %s" % wrap(reads, 11))
        print("    why    %s" % wrap(why, 11))
    print()
    print("kinput: %s" % wrap(
        "two numbers about the harness rather than the kernels. %d spans fit in the %d "
        "bytes it binds, out of the 65534 the struct declares, and associations[] does "
        "not fit at all -- every table above is built inside that. And the fill these "
        "loops were measured hanging on, in iterations instead of adjectives: %d for "
        "fbs_bistochastize_iter's thread 0, and %d pixels per thread for the two "
        "producers." % (SPANS_THAT_FIT, HARNESS_BYTES, pattern_word(20),
                        pattern_word(8) - pattern_word(0)), 8))
    print("kinput: %s" % wrap(
        "air_run has --buffer N@OFF=HEX now, and the lines above are ready to run as "
        "userland/rc.jobs.kinput. Two things about running them. It needs --zero as "
        "well: these bytes say what the control fields hold and nothing about the rest, "
        "so under air_run's byte pattern every field not named here is noise -- which is "
        "how the first run of that file lost nine of its eleven lines. And "
        "metal_drv_test still has no such flag; it takes its flags before the library "
        "path, so one repeatable `--buffer N@OFF=HEX` that memcpy's HEX into buffer N "
        "at byte OFF, after the memset it already does, is the whole change there.", 8))
    print("kinput: %s" % wrap(
        "`kinput.py --check LOG` reads a boot log of that file and says, per kernel, "
        "whether the Adreno wrote what the same input made Apple's own instructions "
        "write on the CPU -- with the canary's boundary first, because after one hung "
        "dispatch this port's GPU stops making writes visible and every later number is "
        "about the boot rather than the kernel.", 8))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
