#!/usr/bin/env python3
"""encodedLength and alignment: Apple's own numbers for an argument buffer.

-[MTLArgumentEncoder encodedLength] is what an APPLICATION allocates its
argument buffer from, and then it encodes at multiples of it. So the number the
driver answers has to be Apple's and not the library's idea of what it needs: one
too small and the application overlaps its own encodings, one too large and it
wastes memory where nothing will notice until something runs out.

tools/mesa/air/air_metal.c answers `8 * the members the set declares` and an
alignment of 8. That is a COMPUTATION, and this is the reading it has to agree
with: the set's own `air.arg_type_size` and `air.arg_type_align_size`, parsed
here out of the module's metadata by a different program in a different
language. Where the two disagree, the library is wrong -- and rwa_module_argbufs
refuses rather than answers for the one shape where they could: a set whose
`[[id(n)]]`s have a gap in them, where Metal's length is over the highest id and
a member count would be too short. This counts how many sets are dense, so that
refusal is known to be unreachable in the library this port is aimed at rather
than assumed to be.

It is a second reading and not a second copy: argbufcheck.py --census counts the
member KINDS against the module's own struct type (and is the authority on
those); this counts the LENGTHS, which that census does not look at.

  argbuflen.py FILE...            .metallib containers or module bitcode
  argbuflen.py --list FILE        one path per line

A `.metallib` container counts as one FILE and all of its functions' sets, which
is why the count is files and not modules: every library of Resident Evil 4 holds
exactly one function (docs/air.md section 8) and Apple's hold several.

Every count whose value is the point when it is zero is printed as a zero rather
than left out: "no set disagrees" and "no counter was reached" are different
measurements, and a missing row cannot tell them apart.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))

NODE = re.compile(r"^!(\d+) = !\{(.*)\}$")
SIZE = re.compile(r'!"air\.arg_type_size", i32 (\d+)')
ALIGN = re.compile(r'!"air\.arg_type_align_size", i32 (\d+)')
BUFSIZE = re.compile(r'!"air\.buffer_size", i32 (\d+)')
STI = re.compile(r'!"air\.struct_type_info", !(\d+)')
MEMBER = re.compile(r'!"air\.indirect_argument", !(\d+)')
LOCATION = re.compile(r'!"air\.location_index", i32 (\d+)')


def module_lengths(ll, counts):
    """One module's sets, counted by their declared length."""
    if "air.indirect_buffer" not in ll:
        return
    nodes = {}
    for line in ll.splitlines():
        m = NODE.match(line.strip())
        if m:
            nodes[int(m.group(1))] = m.group(2)

    for nid, ops in nodes.items():
        # A set node, which is any node that declares `air.indirect_buffer` with
        # a struct type info. The ICB kernel's NESTED set is one of these too --
        # one of 7 858 in Resident Evil 4 -- and is counted here like the rest;
        # docs/air.md 8.2.1 says why that count differs by one from the census's.
        if '!"air.indirect_buffer"' not in ops or '!"air.struct_type_info"' not in ops:
            continue
        counts["sets"] += 1

        sti = STI.search(ops)
        members = MEMBER.findall(nodes.get(int(sti.group(1)), "")) if sti else []
        n = len(members)
        counts["members"] += n

        size = SIZE.search(ops)
        align = ALIGN.search(ops)
        bufsize = BUFSIZE.search(ops)
        if not size:
            counts["sets with no air.arg_type_size"] += 1
        elif int(size.group(1)) == 8 * n:
            counts["sets whose air.arg_type_size is 8 x members"] += 1
        else:
            counts["sets whose air.arg_type_size is NOT 8 x members"] += 1
            counts["  (%s bytes for %d members)" % (size.group(1), n)] += 1

        if not align:
            counts["sets with no air.arg_type_align_size"] += 1
        elif int(align.group(1)) == 8:
            counts["sets whose air.arg_type_align_size is 8"] += 1
        else:
            counts["sets whose alignment is %s and not 8" % align.group(1)] += 1

        # air.buffer_size on the set's own node is the same number again, which
        # is a third statement of it in the same metadata and is counted apart:
        # the frontend reads neither, so an agreement here is evidence about
        # Apple's compiler rather than about ours.
        if bufsize and int(bufsize.group(1)) == 8 * n:
            counts["sets whose air.buffer_size is 8 x members as well"] += 1
        elif bufsize:
            counts["sets whose air.buffer_size differs from 8 x members"] += 1

        ids = []
        for mid in members:
            loc = LOCATION.search(nodes.get(int(mid), ""))
            ids.append(int(loc.group(1)) if loc else None)
        if ids and ids == list(range(n)):
            counts["sets whose ids are 0..n-1 with no gap"] += 1
        else:
            counts["sets whose ids have a GAP or a repeat"] += 1
            counts["  (ids %s)" % ids] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--list", help="a file of paths, one per line")
    args = ap.parse_args()

    paths = list(args.files)
    if args.list:
        with open(args.list) as f:
            paths += [l.strip() for l in f if l.strip()]
    if not paths:
        ap.error("no modules given")

    counts = collections.Counter({
        "sets whose air.arg_type_size is NOT 8 x members": 0,
        "sets with no air.arg_type_size": 0,
        "sets with no air.arg_type_align_size": 0,
        "sets whose ids have a GAP or a repeat": 0,
        "sets whose air.buffer_size differs from 8 x members": 0,
        "files the reader could not print": 0,
    })
    for path in paths:
        q = subprocess.run([AIR2NIR, "--ll", path], capture_output=True, text=True)
        if q.returncode:
            counts["files the reader could not print"] += 1
            continue
        counts["files"] += 1
        module_lengths(q.stdout, counts)

    width = max((len(k) for k in counts), default=10)
    for k in sorted(counts, key=lambda k: (-counts[k], k)):
        print("  %*s  %d" % (width, k, counts[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
