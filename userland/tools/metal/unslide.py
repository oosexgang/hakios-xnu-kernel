#!/usr/bin/env python3
"""Symbolicate the two stack printers this port has, each by its own convention.

A boot log carries stacks from two printers, and they do not agree on what an
address is:

  * `userabort: NAME[pid] aborts, shared cache slide 0xS; called from: A B C ...`
    -- the kernel (`xnu/files/osfmk/arm64/user_exception_trace.c`).  It walks the
    frame pointer chain and prints each saved LR **raw**, that is at its runtime
    address, and prints the slide separately in the same line.  So every frame
    needs 0xS subtracted, and 0xS must be read from that line: it is a different
    number every boot.
  * `redwood: signal N at 0xRAW, pc 0xP, lr 0xL, called from A B C ... (unslid)`
    -- our own handler (`userland/src/inject.c`).  It has already subtracted the
    slide, so nothing may be subtracted twice.  The `at` address is `si_addr`,
    which for a SIGABRT raised out of a syscall is the pc and so states that
    boot's slide, but for a SIGSEGV is the address that could not be touched and
    states nothing.  So the slide is derived from it only when the difference
    looks like a slide at all -- positive, below 4 GiB and page aligned -- and
    even then it is reported as conditional rather than used for anything.

Mixing them is not a near miss.  A frame of the kernel's form resolves, without
any subtraction, inside the un-slid cache -- `a2s` finds the nearest symbol and
answers with confidence, and that answer was `WebCore` and `libwebrtc` in a
window-server stack (docs/metal.md 10.42, 10.45).  Subtracting last boot's slide
instead of this line's gives a different wrong library.  Both look like answers.

So this tool never takes a slide from its caller.  It reads the slide out of the
line that carries the frames, and it says which convention it used.

Frames that are not in the shared cache are reported as such rather than
resolved: the process's own text (around 0x1_0000_0000) is slid by its own ASLR,
and subtracting the cache slide from it produces a number in no image at all.

usage: unslide.py [FILE ...]        # or stdin; every stack line in them
       unslide.py --line 'userabort: ... called from: 0x... 0x...'
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import annot  # noqa: E402  -- memoised a2s and the cache path live there

KERNEL = re.compile(
    r"userabort:\s+(?P<name>\S+)\[(?P<pid>\d+)\]\s+aborts,\s+"
    r"shared cache slide\s+(?P<slide>0x[0-9a-f]+);\s+called from:(?P<frames>[\s0-9a-fx]+)")

OURS = re.compile(
    r"redwood:\s+signal\s+(?P<sig>\d+)\s+at\s+(?P<raw>0x[0-9a-f]+),\s+"
    r"pc\s+(?P<pc>0x[0-9a-f]+),\s+lr\s+(?P<lr>0x[0-9a-f]+),\s+"
    r"called from(?P<frames>[\s0-9a-fx]+)\(unslid\)")

# The cache of this device starts at 0x180000000 and the last subcache ends well
# below 0x280000000; a frame outside that is the process's own text or a thread
# stack, and the cache slide says nothing about it.  The bound is deliberately
# loose: its only job is to keep a2s from answering about an address that is not
# the cache's at all.
CACHE_LO, CACHE_HI = 0x180000000, 0x280000000


def name(addr):
    if not CACHE_LO <= addr < CACHE_HI:
        return None, "not in the shared cache (the process's own text?)"
    sym, seg = annot.a2s(addr)
    if sym == "?" or sym.startswith("sub_"):
        hop, seg2 = annot.name_branch(addr)
        if hop != sym:
            return hop, seg2
    # A frame is a RETURN address, so it points just after a bl and can never be
    # a function's first instruction.  `a2s` answers with the nearest symbol at
    # or below the address, so offset 0 means it reached past the real function
    # into the next one -- which is how a frame inside _MTLReportFailure.cold.1
    # came back named MTLGetEnvCase<MTLErrorModeType>.cold.1 (docs/metal.md
    # 10.46).  Say so rather than print the wrong name as an answer.
    if sym not in (None, "?") and "+" not in sym:
        seg = (seg + "  <- offset 0: a return address is never a function start, "
               "so this name is the NEXT symbol; disassemble back for the caller").strip()
    return sym, seg


def report(kind, slide, frames, extra=()):
    print("%s -- %s" % (kind, "frames are raw: %s subtracted here" % hex(slide)
                        if slide else "frames are already unslid, nothing subtracted"))
    for label, raw in list(extra) + [("frame[%d]" % i, a) for i, a in enumerate(frames)]:
        unslid = raw - slide if slide else raw
        sym, seg = name(unslid)
        where = seg.split("dylib=")[-1].split()[0] if "dylib=" in seg else seg
        print("  %-9s %-13s -> %-13s %-52s %s" % (
            label, hex(raw), hex(unslid), sym or "--", where))
    print()


def main():
    args = sys.argv[1:]
    if args[:1] == ["--line"]:
        text = "\n".join(args[1:])
    elif args:
        text = "".join(open(a, "rb").read().decode("utf-8", "replace") for a in args)
    else:
        text = sys.stdin.buffer.read().decode("utf-8", "replace")

    seen, n = set(), 0
    for line in text.splitlines():
        m = KERNEL.search(line)
        if m:
            slide = int(m.group("slide"), 16)
            frames = tuple(int(f, 16) for f in m.group("frames").split())
            # Process text differs per restart, so the identity of a stack is
            # what is in the cache: without this a crash loop prints forty
            # identical reports that differ in one frame.
            key = ("k", slide, tuple(f for f in frames if CACHE_LO <= f - slide < CACHE_HI))
            if key in seen:
                continue
            seen.add(key)
            n += 1
            report("kernel userabort: %s[%s]" % (m.group("name"), m.group("pid")),
                   slide, frames)
            continue
        m = OURS.search(line)
        if m:
            pc, raw = int(m.group("pc"), 16), int(m.group("raw"), 16)
            frames = tuple(int(f, 16) for f in m.group("frames").split())
            key = ("r", m.group("sig"), frames, pc)
            if key in seen:
                continue
            seen.add(key)
            n += 1
            # si_addr is the pc for an abort out of a syscall, and then the line
            # states its own boot's slide -- but it is the inaccessible address
            # for a fault, and then the difference means nothing.  Reported when
            # it could be a slide, never applied to anything.
            d = raw - pc
            said = ("this boot's slide, if `at` is the pc: %s" % hex(d)
                    if 0 < d < 0x100000000 and d % 0x4000 == 0
                    else "fault address %s, no slide derivable" % hex(raw))
            report("redwood: signal %s (%s)" % (m.group("sig"), said), 0, frames,
                   extra=[("pc", pc), ("lr", int(m.group("lr"), 16))])
            continue
    annot._save()
    if not n:
        print("unslide.py: no stack line of either printer found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
