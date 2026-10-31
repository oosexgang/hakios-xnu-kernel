#!/usr/bin/env python3
"""Which Metal selectors an application actually SENDS, as opposed to names.

appasks.py counts the selectors whose STRING is in an application's binary, and
says plainly that a bare word is weak evidence.  This tool exists because for a
metal-cpp application the colon-bearing selectors are weak evidence too, and
that is not a detail: it is the difference between "RE4 asks -supportsFamily:"
and "RE4 does not".

metal-cpp registers its whole selector table in ONE static initializer.  The
shape, read out of Resident Evil 4 at 0x109452898 and identical for all 1 238 of its
entries:

    adrp x0, <__cstring page>        ; the selector's text
    add  x0, x0, #0x57a              ; "supportsFamily:"
    bl   <__stubs>                   ; sel_registerName
    adrp x8, 0x111342000
    str  x0, [x8, #0x5b8]            ; MTL::Private::Selector::s_kSupportsFamily

Every selector metal-cpp knows is registered whether the program sends it or
not, because the table is one translation unit with no dead-code elimination
across it.  So the string proves the TABLE, and what proves a send is a SECOND
reference: a load of that cached SEL from somewhere other than the initializer,
which is what Object::sendMessage() compiles to.  Zero loads means registered
and never sent.

Three forms of load are counted, because missing one would read as "never sent":

    adrp x8, page;  ldr x1, [x8, #off]              the common one
    adrp x8, page;  ldp x1, x2, [x8, #off]          two adjacent selectors at once
    adrp x20, page; add x20, x20, #off; ldr x1, [x20]   address hoisted out of a loop

and the window after an adrp ends as soon as anything writes the register it
loaded, which is not a nicety: without it `add x20, x20, #0xd18` followed by
`ldr x1, [x20]` was counted as seventeen sends of -isLowPower, and -isLowPower
is exactly the kind of answer one would then go and "fix".

What this CANNOT establish, and must not be read as establishing:

  * that a selector with loads is sent at startup, or on any particular path.
    It is sent somewhere in 200 MB of code; which code is a separate question;
  * that a selector with no loads is never sent.  A send through a SEL loaded by
    a form not listed above, or through a pointer computed at runtime, or from a
    library the application dlopens, would not appear.  The honest reading of
    zero is "no send site in this binary that this tool can see";
  * anything at all about a selector whose string is an ordinary word.  "name"
    occurs 23 times in RE4's __cstring as something other than a selector, so a
    bare word is reported with its ambiguity rather than a number.

usage:
  appsends.py BINARY [selector ...]          # named selectors
  appsends.py BINARY --table                 # every entry of the registered table
  appsends.py BINARY --decl Metal.objc.txt   # only what Apple's Metal declares
"""
import argparse
import os
import re
import struct
import sys

import numpy as np


class MachO:
    """Just enough Mach-O to find the sections, the stubs and their symbols."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        magic, = struct.unpack('<I', self.f.read(4))
        if magic != 0xFEEDFACF:
            sys.exit('appsends: not a 64-bit little-endian Mach-O (magic %#x)' % magic)
        self.f.seek(0)
        hdr = self.f.read(32)
        ncmds, = struct.unpack_from('<I', hdr, 16)
        self.sections = {}
        self.stubs = None
        self.symtab = self.dysymtab = None
        off = 32
        for _ in range(ncmds):
            self.f.seek(off)
            cmd, size = struct.unpack('<II', self.f.read(8))
            body = self.f.read(size - 8)
            if cmd == 0x19:                                     # LC_SEGMENT_64
                nsects, = struct.unpack_from('<I', body, 56)
                for i in range(nsects):
                    s = body[64 + i * 80: 144 + i * 80]
                    name = s[0:16].rstrip(b'\0').decode()
                    seg = s[16:32].rstrip(b'\0').decode()
                    addr, sz, fileoff, _align, _reloff, _nreloc, flags, r1 = \
                        struct.unpack_from('<QQIIIIII', s, 32)
                    self.sections['%s,%s' % (seg, name)] = (addr, sz, fileoff, flags, r1)
            elif cmd == 0x2:                                    # LC_SYMTAB
                self.symtab = struct.unpack_from('<IIII', body, 0)
            elif cmd == 0xB:                                    # LC_DYSYMTAB
                self.dysymtab = struct.unpack_from('<' + 'I' * 18, body, 0)
            off += size

    def read(self, fileoff, size):
        self.f.seek(fileoff)
        return self.f.read(size)

    def section(self, name):
        if name not in self.sections:
            sys.exit('appsends: no section %s' % name)
        return self.sections[name]

    def stub_names(self):
        """{stub address: symbol name} through the indirect symbol table.

        This is what names the call in the registration pattern rather than
        assuming it: the stub the initializer branches to must BE
        sel_registerName, and nothing else in the pattern says so.
        """
        if '__TEXT,__stubs' not in self.sections or not (self.symtab and self.dysymtab):
            return {}
        addr, size, _off, _flags, r1 = self.section('__TEXT,__stubs')
        symoff, nsyms, stroff, strsize = self.symtab
        indirectsymoff, nindirectsyms = self.dysymtab[12], self.dysymtab[13]
        syms = self.read(symoff, nsyms * 16)
        strs = self.read(stroff, strsize)
        ind = np.frombuffer(self.read(indirectsymoff, nindirectsyms * 4), dtype='<u4')
        out = {}
        for i in range(size // 12):                             # arm64 stub: adrp/add/br
            k = r1 + i
            if k >= len(ind):
                break
            si = int(ind[k])
            if si >= nsyms:
                continue
            strx, = struct.unpack_from('<I', syms, si * 16)
            end = strs.find(b'\0', strx)
            out[addr + i * 12] = strs[strx:end].decode(errors='replace')
        return out


# --- instruction decoding, only the forms the pattern uses ---

def adrp_page(word, pc):
    lo = (word >> 29) & 3
    hi = (word >> 5) & 0x7FFFF
    imm = (hi << 2) | lo
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~0xFFF) + imm * 4096


def is_adrp(w):
    return (w & 0x9F000000) == 0x90000000


def add_imm64(w):
    return (w & 0xFF800000) == 0x91000000, (w >> 10) & 0xFFF, (w >> 5) & 31, w & 31


def ldr_u64(w):
    return (w & 0xFFC00000) == 0xF9400000, ((w >> 10) & 0xFFF) * 8, (w >> 5) & 31, w & 31


def ldp_64(w):
    ok = (w & 0xFFC00000) == 0xA9400000
    imm = (w >> 15) & 0x7F
    if imm & 0x40:
        imm -= 0x80
    return ok, imm * 8, (w >> 5) & 31, w & 31


def writes_reg(w):
    """the destination register of the forms that could clobber an address, or None.

    Stores and branches write no register; everything else here is treated as
    writing bits 4-0, which is true of the data-processing and load forms that
    turn up between an adrp and the load that uses it.
    """
    if (w & 0xFFC00000) in (0xF9000000, 0xB9000000, 0x39000000, 0x79000000):   # STR
        return None
    if (w & 0xFFC00000) == 0xA9000000:                                          # STP
        return None
    if (w & 0x7C000000) == 0x14000000:                                          # B/BL
        return None
    if (w & 0xFE000000) in (0x34000000, 0x36000000):                            # CBZ/TBZ
        return None
    return w & 31


class Text:
    def __init__(self, macho):
        addr, size, fileoff, _flags, _r1 = macho.section('__TEXT,__text')
        self.va = addr
        self.code = np.frombuffer(macho.read(fileoff, size & ~3), dtype='<u4')
        self.n = len(self.code)
        pcs = self.va + 4 * np.arange(self.n, dtype=np.int64)
        m = (self.code & np.uint32(0x9F000000)) == np.uint32(0x90000000)
        self.adrp_idx = np.flatnonzero(m)
        w = self.code[self.adrp_idx].astype(np.int64)
        lo = (w >> 29) & 3
        hi = (w >> 5) & 0x7FFFF
        imm = (hi << 2) | lo
        imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
        self.adrp_page = (pcs[self.adrp_idx] & ~np.int64(0xFFF)) + imm * 4096
        self.adrp_rd = (w & 31)
        self.by_page = {}
        for k in range(len(self.adrp_idx)):
            self.by_page.setdefault(int(self.adrp_page[k]), []).append(k)

    def pc(self, i):
        return self.va + 4 * i

    def idx(self, pc):
        return (pc - self.va) // 4

    def materialisations(self, addr, window=10):
        """every (adrp pc, add pc, register) that puts `addr` in a register"""
        out = []
        for k in self.by_page.get(addr & ~0xFFF, []):
            i, r = int(self.adrp_idx[k]), int(self.adrp_rd[k])
            for j in range(i + 1, min(i + 1 + window, self.n)):
                w = int(self.code[j])
                ok, imm, rn, rd = add_imm64(w)
                if ok and rn == r and imm == (addr & 0xFFF):
                    out.append((self.pc(i), self.pc(j), rd))
                    break
                if writes_reg(w) == r:
                    break
        return out

    def loads_of(self, addr, window=12):
        """every pc that loads the 64-bit word at `addr`, in the three forms."""
        out = set()
        off = addr & 0xFFF
        for k in self.by_page.get(addr & ~0xFFF, []):
            i, r = int(self.adrp_idx[k]), int(self.adrp_rd[k])
            for j in range(i + 1, min(i + 1 + window, self.n)):
                w = int(self.code[j])
                ok, imm, rn, _rd = ldr_u64(w)
                if ok and rn == r and imm == off:
                    out.add(self.pc(j))
                    break
                ok, imm, rn, _rd = ldp_64(w)
                if ok and rn == r and imm in (off, off - 8):
                    out.add(self.pc(j))
                    break
                ok, imm, rn, rd = add_imm64(w)
                if ok and rn == r:
                    # the address was hoisted: look for a load off the new register
                    base = (addr & ~0xFFF) + imm
                    for j2 in range(j + 1, min(j + 1 + window, self.n)):
                        w2 = int(self.code[j2])
                        ok2, imm2, rn2, _ = ldr_u64(w2)
                        if ok2 and rn2 == rd and base + imm2 == addr:
                            out.add(self.pc(j2))
                            break
                        if writes_reg(w2) == rd:
                            break
                    break
                if writes_reg(w) == r:
                    break
        return out


def cstrings(macho, section='__TEXT,__cstring'):
    addr, size, fileoff, _flags, _r1 = macho.section(section)
    blob = macho.read(fileoff, size)
    out = {}
    pos = 0
    for part in blob.split(b'\0'):
        if part:
            try:
                out.setdefault(part.decode('ascii'), []).append(addr + pos)
            except UnicodeDecodeError:
                pass
        pos += len(part) + 1
    return out


def registration_table(macho, text, stub_names):
    """{selector: (global address, registration pc)} for the metal-cpp table.

    Built from the pattern rather than from a list of selectors, so the table's
    size is a measurement: a binary that does not do this has none.
    """
    regstubs = {a for a, n in stub_names.items() if n.lstrip('_') == 'sel_registerName'}
    if not regstubs:
        return {}, None
    cs = cstrings(macho)
    addr_to_str = {}
    for s, addrs in cs.items():
        for a in addrs:
            addr_to_str[a] = s
    table, pcs = {}, []
    # walk the text once: bl to the stub, then read backwards for the adrp/add and
    # forwards for the adrp/str
    bl = (text.code & np.uint32(0xFC000000)) == np.uint32(0x94000000)
    for i in np.flatnonzero(bl):
        i = int(i)
        w = int(text.code[i])
        imm = w & 0x3FFFFFF
        if imm & (1 << 25):
            imm -= 1 << 26
        if text.pc(i) + imm * 4 not in regstubs:
            continue
        # backwards: add x0, x0, #off ; adrp x0, page
        if i < 2:
            continue
        ok, off, rn, rd = add_imm64(int(text.code[i - 1]))
        if not (ok and rd == 0 and rn == 0 and is_adrp(int(text.code[i - 2]))):
            continue
        page = adrp_page(int(text.code[i - 2]), text.pc(i - 2))
        sel = addr_to_str.get(page + off)
        if sel is None:
            continue
        # forwards: adrp xN, page ; str x0, [xN, #off]
        g = None
        pagereg = {}
        for j in range(i + 1, min(i + 8, text.n)):
            w2 = int(text.code[j])
            if is_adrp(w2):
                pagereg[w2 & 31] = adrp_page(w2, text.pc(j))
                continue
            if (w2 & 0xFFC00000) == 0xF9000000:
                imm2 = ((w2 >> 10) & 0xFFF) * 8
                rn2, rt2 = (w2 >> 5) & 31, w2 & 31
                if rt2 == 0 and rn2 in pagereg:
                    g = pagereg[rn2] + imm2
                break
        if g is not None:
            table[sel] = (g, text.pc(i - 1))
            pcs.append(text.pc(i))
    return table, (min(pcs), max(pcs)) if pcs else None


def declared(paths):
    meth = re.compile(r'^\s*[-+]\[(\S+)\s+([^\]]+)\];')
    out = {}
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = meth.match(line)
                if m:
                    out.setdefault(m.group(2).strip(), set()).add(m.group(1).strip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('selectors', nargs='*')
    ap.add_argument('--table', action='store_true', help='every registered selector')
    ap.add_argument('--decl', action='append', default=[],
                    help='an `ipsw dyld macho --objc` dump; restricts --table to what it declares')
    ap.add_argument('--sent-only', action='store_true')
    a = ap.parse_args()

    macho = MachO(a.binary)
    stubs = macho.stub_names()
    text = Text(macho)
    table, span = registration_table(macho, text, stubs)
    print('%s: %d instructions of __text, %d selectors registered through %s'
          % (os.path.basename(a.binary), text.n, len(table),
             'sel_registerName' if table else 'nothing -- no metal-cpp table here'))
    if span:
        print('  the table\'s registrations run from %#x to %#x' % span)
    if not table:
        return
    decl = declared(a.decl) if a.decl else None
    want = a.selectors
    if a.table or not want:
        want = sorted(table)
        if decl:
            want = [s for s in want if s in decl]
    print()
    print('  %-52s %-14s %s' % ('selector', 'SEL global', 'send sites'))
    nsent = 0
    for s in want:
        if s not in table:
            print('  %-52s %-14s not in the table' % (s, '-'))
            continue
        g, _pc = table[s]
        sites = sorted(text.loads_of(g))
        nsent += 1 if sites else 0
        if a.sent_only and not sites:
            continue
        note = ' '.join(hex(x) for x in sites[:4]) + (' ...' if len(sites) > 4 else '')
        print('  %-52s %-14s %-4d %s' % (s, hex(g), len(sites), note))
    print()
    print('  %d of %d asked about have a send site; the rest are registered and, as far as'
          % (nsent, len([s for s in want if s in table])))
    print('  this binary shows, never sent. Read the docstring before reading that as never.')


if __name__ == '__main__':
    main()
