#!/usr/bin/env python3
"""Annotate a window of cache disassembly with the names behind its addresses.

`ipsw dyld disass` can symbolicate branch targets itself, but only with its
markup pass on -- and that pass parses the whole cache's ObjC metadata and runs
this machine out of memory (docs/metal.md 10.1).  So the window is dumped with
--quiet, which is bounded and cheap, and every address in it is resolved
afterwards one at a time: `a2s` costs about half a second and the answers are
memoised, so a 1000-instruction function annotates once and re-reads for free.

Three kinds of address are worth naming, and each needs a different question:

  * a branch target -- `a2s` names it.  Calls through __TEXT_STUBS have no
    symbol of their own, so a stub is disassembled one hop further and the
    name of its destination is reported instead;
  * `adrp`/`add` -- a literal address.  Most of the interesting ones in
    Objective-C++ are C strings, so the bytes are dumped and shown when they
    are printable;
  * `adrp`/`ldr` -- a slot holding a pointer: a selector reference, a class
    reference, a got entry.  The pointer is read and then named.

Usage: annot.py ADDR COUNT [more ADDR COUNT pairs...]
"""

import json
import os
import re
import subprocess
import sys

CACHE = os.environ.get(
    "DSC", os.path.join(os.path.dirname(__file__), "..", "..", "ios165",
                        "20F66__iPhone10,3_6", "dyld_shared_cache_arm64"))
MEMO = os.environ.get("ANNOT_CACHE", "/tmp/annot-memo.json")

_memo = {}
if os.path.exists(MEMO):
    try:
        _memo = json.load(open(MEMO))
    except ValueError:
        _memo = {}


def _save():
    try:
        json.dump(_memo, open(MEMO, "w"))
    except OSError:
        pass


def ipsw(args, key):
    if key in _memo:
        return _memo[key]
    out = subprocess.run(["ipsw", "dyld"] + args, capture_output=True,
                         text=True).stdout
    _memo[key] = out
    return out


def disass(addr, count):
    return ipsw(["disass", CACHE, "-q", "-a", hex(addr), "-c", str(count)],
                "dis:%x:%d" % (addr, count))


def a2s(addr):
    out = ipsw(["a2s", CACHE, hex(addr)], "a2s:%x" % addr)
    sym, seg = "?", ""
    for line in out.splitlines():
        m = re.match(r"^0x[0-9a-f]+:\s*(.*)$", line.strip())
        if m:
            sym = m.group(1).strip()
        m = re.search(r"(dylib=\S*|segment=\S+|section=\S+)", line)
        if m:
            seg += " " + line.strip().split("•")[-1].strip()
    return sym, seg.strip()


def dump_bytes(addr, size=96):
    out = ipsw(["dump", CACHE, hex(addr), "-s", str(size)],
               "dmp:%x:%d" % (addr, size))
    data = bytearray()
    for line in out.splitlines():
        # the hexdump splits each 16-byte row into two groups separated by two
        # spaces, so a "(XX )+" pattern stops after the first eight bytes and
        # quietly returns every other half of a string.  Take the whole row.
        m = re.match(r"^[0-9a-f]{16}:\s+([0-9a-f ]+?)\s*\|", line.strip())
        if m:
            data += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(data)


def cfstring(addr):
    """A constant NSString in this cache: flags, data pointer, length, isa.

    The field order was read off a known literal rather than assumed, so each
    8-byte field is tried as a pointer and the first printable one wins -- that
    survives a different order in some other cache without a silent wrong
    answer.
    """
    data = dump_bytes(addr, 32)
    if len(data) < 32:
        return None
    for off in (8, 16, 24, 0):
        ptr = int.from_bytes(data[off:off + 8], "little") & CHAINED
        if ptr < 0x100000000:
            continue
        s = cstring(ptr)
        if s:
            return s
    return None


def cstring(addr):
    data = dump_bytes(addr)
    if not data:
        return None
    end = data.find(b"\0")
    s = data[: end if end >= 0 else len(data)]
    if not s:
        return ""
    if all(0x20 <= c < 0x7F or c in (9, 10) for c in s):
        return s.decode()
    return None


# A pointer in this cache is a chained fixup: the address it means is the low 36
# bits (0x00000401_8857eeb8 means 0x18857eeb8), and the rest is the fixup's own
# bookkeeping.  A 48-bit mask leaves that bookkeeping in place and names nothing.
CHAINED = 0xFFFFFFFFF


def quadword(addr):
    data = dump_bytes(addr, 8)
    if len(data) < 8:
        return None
    return int.from_bytes(data[:8], "little")


def name_branch(target):
    """a2s, but one hop through a stub so a stubbed call still has a name."""
    sym, seg = a2s(target)
    if sym != "?" and not sym.startswith("sub_"):
        return sym, seg
    hop = disass(target, 4)
    # a stub is adrp/add/br or ldr/br; the br's register is loaded from a got
    # slot, so read the slot instead of following the branch.
    m = re.search(r"adrp\s+x\d+, (0x[0-9a-f]+)", hop)
    n = re.search(r"(?:ldr|add)\s+x\d+, x\d+,? ?#?(0x[0-9a-f]+)", hop)
    if m and n:
        slot = int(m.group(1), 16) + int(n.group(1), 16)
        if "ldr" in hop:
            ptr = quadword(slot)
            if ptr:
                s2, g2 = a2s(ptr & CHAINED)
                return "stub -> " + s2, g2
        else:
            s2, g2 = a2s(slot)
            return "stub -> " + s2, g2
    return sym, seg


def main():
    args = sys.argv[1:]
    if len(args) < 2 or len(args) % 2:
        print(__doc__)
        return 1
    for i in range(0, len(args), 2):
        base, count = int(args[i], 16), int(args[i + 1])
        text = disass(base, count)
        pending = {}      # register -> (page, line index) from the last adrp
        for line in text.splitlines():
            m = re.match(r"^(0x[0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(.*)$",
                         line)
            if not m:
                print(line)
                continue
            addr, insn = int(m.group(1), 16), m.group(2).strip()
            note = ""
            b = re.match(r"^(bl|b|cbz|cbnz|tbz|tbnz|b\.\w+)\s+.*?(0x[0-9a-f]+)$",
                         insn)
            if b and b.group(1) in ("bl", "b"):
                target = int(b.group(2), 16)
                if not (base <= target < base + count * 4):
                    sym, seg = name_branch(target)
                    note = "  ; %s   [%s]" % (sym, seg)
            ad = re.match(r"^adrp\s+(x\d+), (0x[0-9a-f]+)$", insn)
            if ad:
                pending[ad.group(1)] = int(ad.group(2), 16)
            ar = re.match(r"^add\s+(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$", insn)
            if ar and ar.group(2) in pending:
                off = int(ar.group(3), 16) if ar.group(3).startswith("0x") \
                    else int(ar.group(3))
                ea = pending[ar.group(2)] + off
                sym, seg = a2s(ea)
                s = cstring(ea)
                cf = cfstring(ea) if not s else None
                note = "  ; %s" % hex(ea)
                if s:
                    note += ' = "%s"' % s
                elif cf:
                    note += ' = @"%s"' % cf
                elif sym != "?":
                    note += " = " + sym
                elif seg:
                    note += " = " + seg.split("section=")[-1]
            lr = re.match(r"^ldr\s+(x\d+), \[(x\d+),? ?#?(0x[0-9a-f]+|\d+)?\]$",
                          insn)
            if lr and lr.group(2) in pending:
                g = lr.group(3) or "0"
                off = int(g, 16) if g.startswith("0x") else int(g)
                ea = pending[lr.group(2)] + off
                ptr = quadword(ea)
                note = "  ; slot %s" % hex(ea)
                if ptr:
                    masked = ptr & CHAINED
                    s = cstring(masked)
                    sym, _ = a2s(masked)
                    if s is not None:
                        note += ' -> "%s"' % s
                    elif sym != "?":
                        note += " -> " + sym
                    else:
                        note += " -> " + hex(masked)
            print("%s:  %-44s%s" % (hex(addr), insn, note))
    _save()
    return 0


if __name__ == "__main__":
    sys.exit(main())
