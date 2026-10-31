#!/usr/bin/env python3
"""Which kernels need structure in their buffers, and so cannot be dispatched here.

`air_run` fills every buffer a kernel reads with `(i * 7 + 13) & 0xff`. For a
kernel that treats those bytes as numbers that is a fine input: the result is
arbitrary but computable, and the checks compare it against LLVM or against the
folder. For a kernel that treats them as *control data* -- a count, an index
into another buffer, a sentinel that ends a traversal -- it is not an input at
all. Two such kernels were measured as "hanging the GPU" before anyone read
them: `fbs_bistochastize_iter` counts from one `HashEntry` field to another and
leaves the loop only on equality, and `MetalFaceMask_GenerateToothMask` walks a
span table until it meets `-1`. Neither value occurs in the pattern, so neither
loop ends, and the GPU faithfully runs forever (docs/air.md section 5).

This separates the two cases by reading the module, so that the kernel table can
say which of its unfinished rows are unfinishable *by this harness* rather than
unexplained. The signals are deliberately crude and named rather than combined
into a verdict:

  loop-exit-on-loaded   a loop whose exit test compares against a value that
                        traces back to a load from a buffer (address space 1 or
                        2). That is the shape that does not terminate.
  indirect-index        a buffer indexed by a value loaded from a buffer, so the
                        address itself comes from the pattern.

A kernel with neither signal can be given the pattern and will finish; a kernel
with either cannot be measured here at all, and no number of boots changes
that. What would is a harness that knows what each buffer means -- which is the
runtime's knowledge, not the compiler's.

`tools/air/kinput.py` is that knowledge for eleven of the fourteen: which value
each loop leaves on, which buffer and byte offset it is loaded from, and the
bytes that end it. It also reads one of the fourteen back the other way --
`confidence` leaves its loops on values that are clamped to constants before
the loop uses them, so the signal below over-reports it and no input is needed.
The signal is deliberately crude; that is the shape of its crudeness.

usage: kstruct.py [FILE.metallib...]       (no files: the corpus's five)
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))

LIBS = [
    "ios165/rootfs/System/Library/VideoDecoders/AV1SW.metallib",
    "ios165/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib",
    "ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib",
    "ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/portrait_filters.metallib",
]

DEPTH = 6   # how far back a value is traced; the shapes of interest are shallow


def defs_of(ll):
    """{ssa name: the text that defines it}."""
    out = {}
    for line in ll.splitlines():
        m = re.match(r"\s*%([A-Za-z_0-9.]+) = (.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def from_buffer(name, defs, depth=DEPTH):
    """Does this value trace back to a load from address space 1 or 2?"""
    if depth <= 0 or name not in defs:
        return False
    text = defs[name]
    if re.match(r"load .*addrspace\([12]\)", text):
        return True
    # a phi, a cast, an arithmetic op: follow every operand it names
    return any(from_buffer(n, defs, depth - 1)
               for n in re.findall(r"%([A-Za-z_0-9.]+)", text))


def loops_and_signals(ll):
    """(number of loops, loop-exit-on-loaded, indirect-index) for one module."""
    defs = defs_of(ll)
    # This reads our own printer's output (air2nir --ll), which prints a block as
    # "N:" with no predecessor comment -- llvm-dis adds one, we do not -- so the
    # back edges are found from the branches rather than from a preds list.
    blocks = {}
    for m in re.finditer(r"^(\d+):$(.*?)(?=^\d+:$|\Z)", ll, re.M | re.S):
        blocks[int(m.group(1))] = m.group(2)
    targets = {blk: [int(x) for x in re.findall(r"label %(\d+)", body)]
               for blk, body in blocks.items()}

    nloops, exit_loaded = 0, False
    for blk, body in blocks.items():
        back = [t for t in targets[blk] if t <= blk]
        if not back:
            continue
        nloops += 1
        # the test this block leaves the loop on
        for m in re.finditer(r"br i1 %([A-Za-z_0-9.]+),", body):
            t = defs.get(m.group(1), "")
            for n in re.findall(r"%([A-Za-z_0-9.]+)", t):
                if from_buffer(n, defs):
                    exit_loaded = True

    indirect = False
    for name, text in defs.items():
        if not text.startswith("getelementptr"):
            continue
        # the indices follow the base pointer; a loaded index means the address
        # itself comes out of a buffer
        ops = re.findall(r"%([A-Za-z_0-9.]+)", text)
        for n in ops[1:]:
            if from_buffer(n, defs):
                indirect = True
    return nloops, exit_loaded, indirect


def main(argv):
    libs = [a for a in argv if not a.startswith("--")] or LIBS
    if not os.path.exists(AIR2NIR):
        print("kstruct: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1
    rows = []
    for lib in libs:
        path = lib if os.path.isabs(lib) else os.path.join(ROOT, lib)
        if not os.path.exists(path):
            print("kstruct: missing %s" % lib, file=sys.stderr)
            return 1
        names = []
        q = subprocess.run([AIR2NIR, "--list", path], capture_output=True)
        for line in q.stdout.decode(errors="replace").splitlines():
            f = line.split()
            if len(f) >= 2 and f[1] == "kernel":
                names.append(f[0])
        for n in names:
            ll = subprocess.run([AIR2NIR, "--ll", path, n],
                                capture_output=True).stdout.decode(errors="replace")
            rows.append((n,) + loops_and_signals(ll))

    # The two signals do not mean the same thing, and lumping them together said
    # the wrong thing once already: four kernels with a loaded index and no such
    # loop finished on the device perfectly well.
    endless = [r for r in rows if r[2]]
    addressed = [r for r in rows if r[3] and not r[2]]
    plain = [r for r in rows if not (r[2] or r[3])]
    print("kstruct: %d compute kernels" % len(rows))
    print("  %2d  a loop leaves on a value from a buffer: **cannot be dispatched here at all**, "
          "the loop does not end" % len(endless))
    print("  %2d  index a buffer by a value from a buffer, with no such loop: they finish, but "
          "what they compute is addresses out of a byte pattern" % len(addressed))
    print("  %2d  read their buffers as numbers, so the fill pattern is a usable input"
          % len(plain))
    print()
    for title, group in (("the loop does not end", endless),
                         ("addresses out of the pattern", addressed)):
        print("  %s:" % title)
        for n, l, ex, ind in sorted(group):
            print("    %-38s %d loop%s" % (n, l, "" if l == 1 else "s"))
    if len(endless) + len(addressed) + len(plain) != len(rows):
        print("kstruct: the rows do not add up", file=sys.stderr)
        return 1
    print()
    print("kstruct: these are shapes in the module, not verdicts about the kernels. The first "
          "group is a statement about this harness: a loop that leaves on a value from a "
          "buffer does not leave when the buffer holds (i * 7 + 13) & 0xff, and no number of "
          "boots changes that -- it needs a harness that knows what each buffer means.")
    print("kstruct: tools/air/kinput.py is that, for eleven of the first group: the value "
          "each loop leaves on, where it is loaded from, and the bytes that end it. It also "
          "names one row above as this signal over-reporting -- `confidence` clamps its "
          "bounds to constants, so it needs no input at all.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
