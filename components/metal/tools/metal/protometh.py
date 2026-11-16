#!/usr/bin/env python3
"""What a protocol requires, with the type of every method, read out of a Mach-O.

absmeth.py answers "does this class's method do anything"; this answers the
question one level up: "what does this protocol demand, and with what
signature". It exists because the private protocols a driver has to satisfy --
MTLCommandQueueSPI, MTLCommandBufferSPI, MTLRenderCommandEncoderSPI and the rest
-- are NOT defined in Metal.framework. They are defined in the GPU bundle
(AGXMetalA11.bundle on this phone), which never loads on hardware that is not
Apple's, so `objc_getProtocol("MTLCommandQueueSPI")` returns NULL in our
processes and the runtime cannot be asked. The one copy of the list is the cache.

`ipsw dyld macho <DSC> <image> --objc` prints the selectors but not their types,
and the type is the part that cannot be guessed: a method declared to return
void where the caller expects a struct leaves the caller's buffer uninitialised,
and a method declared to return a struct where the caller expects a scalar
writes through whatever x8 happened to hold. So the protocol metadata is parsed
here instead.

usage:
  ipsw dyld extract <DSC> /System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11 --output DIR
  protometh.py DIR/AGXMetalA11 --proto MTLCommandQueueSPI [--proto ...] [--json]
  protometh.py DIR/AGXMetalA11 --list
"""

import argparse
import json
import re
import struct
import sys

LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xFEEDFACF

# address -> selector, filled from `ipsw dyld objc sel <DSC> -i <image>`. The
# shared cache uniques every selector string into a table of its own, outside
# any image, so a method list in an extracted image points at an address no
# extracted image contains. ipsw can print that table per image (the whole
# table at once runs the machine out of memory), and this is where its lines go.
SELMAP = {}


class Image:
    """Just enough Mach-O to turn a vmaddr into bytes."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.buf = f.read()
        magic, = struct.unpack_from("<I", self.buf, 0)
        if magic != MH_MAGIC_64:
            raise ValueError("%s is not a 64-bit little-endian Mach-O (%#x)"
                             % (path, magic))
        ncmds, = struct.unpack_from("<I", self.buf, 16)
        self.segments = []   # (vmaddr, vmsize, fileoff, filesize, name)
        self.sections = {}   # "__SEG,__sect" -> (addr, size, offset)
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.buf, off)
            if cmd == LC_SEGMENT_64:
                segname = self.buf[off + 8:off + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                    "<QQQQ", self.buf, off + 24)
                nsects, = struct.unpack_from("<I", self.buf, off + 64)
                self.segments.append((vmaddr, vmsize, fileoff, filesize, segname))
                soff = off + 72
                for _ in range(nsects):
                    sect = self.buf[soff:soff + 16].rstrip(b"\0").decode()
                    addr, size = struct.unpack_from("<QQ", self.buf, soff + 32)
                    foff, = struct.unpack_from("<I", self.buf, soff + 48)
                    self.sections["%s,%s" % (segname, sect)] = (addr, size, foff)
                    soff += 80
            off += cmdsize

    def abstract_stub(self):
        """The address of this image's `doesNotRecognizeSelector:` objc stub, or 0.

        Found without ipsw: the symbol is in the image's own symbol table when it is
        extracted, and otherwise the stub is named in the selector map."""
        if getattr(self, "_stub", None) is None:
            self._stub = 0
            for addr, name in SELMAP.items():
                if name == "doesNotRecognizeSelector:":
                    self._stub = self._stub_calling(addr)
                    break
        return self._stub

    def _stub_calling(self, selector_addr):
        """The __objc_stubs entry that sends `selector_addr`.

        A stub is `adrp x1, page; ldr x1, [x1, off]; adrp x16..; b _objc_msgSend`, so the
        selector it sends is found by decoding its first two instructions."""
        sect = self.sections.get("__TEXT,__objc_stubs")
        if sect is None:
            return 0
        addr, size, _ = sect
        at = addr
        while at < addr + size:
            w0, w1 = self.u32(at), self.u32(at + 4)
            if w0 is None or w1 is None:
                break
            if (w0 >> 24) & 0x9F == 0x90 and (w1 >> 22) == 0b1111100101:   # adrp + ldr
                immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7FFFF
                imm = (immhi << 2 | immlo)
                if imm & (1 << 20):
                    imm -= 1 << 21
                page = (at & ~0xFFF) + imm * 0x1000
                off = ((w1 >> 10) & 0xFFF) * 8
                if self.u64(page + off) == selector_addr:
                    return at
            at += 4
        return 0

    def link(self, others):
        """Other images to resolve addresses this one does not contain.

        Selector and type strings are uniqued across the whole shared cache, so
        a protocol defined in AGXMetalA11 names its methods with pointers into
        Metal.framework's __objc_methname. Without the neighbour images every
        such selector reads back as nothing."""
        self.others = [o for o in others if o is not self]

    def _find(self, addr):
        for img in [self] + getattr(self, "others", []):
            for vmaddr, vmsize, fileoff, filesize, _ in img.segments:
                if vmaddr <= addr < vmaddr + vmsize:
                    delta = addr - vmaddr
                    return (img, fileoff + delta) if delta < filesize else (None, None)
        return (None, None)

    def off(self, addr):
        img, o = self._find(addr)
        return o if img is self else None

    def u32(self, addr):
        img, o = self._find(addr)
        return None if o is None else struct.unpack_from("<I", img.buf, o)[0]

    def i32(self, addr):
        img, o = self._find(addr)
        return None if o is None else struct.unpack_from("<i", img.buf, o)[0]

    def u64(self, addr):
        img, o = self._find(addr)
        if o is None:
            return None
        v, = struct.unpack_from("<Q", img.buf, o)
        # Pointers in an extracted cache image are still chained fixups: the low
        # 40 bits are the target address and the bits above them are the chain's
        # own metadata (0x20212afb3e0 means 0x212afb3e0). No address in this
        # cache needs more than 40 bits, so masking is unconditional and a plain
        # pointer survives it unchanged.
        return v & 0xFFFFFFFFFF

    def cstr_start(self, addr, limit=512):
        """Like cstr, but only when `addr` is the first byte of a string."""
        img, o = self._find(addr)
        if o is None or o == 0 or img.buf[o - 1] != 0:
            return None
        return self.cstr(addr, limit)

    def cstr(self, addr, limit=512):
        img, o = self._find(addr)
        if o is None:
            return SELMAP.get(addr)
        end = img.buf.find(b"\0", o, o + limit)
        if end < 0:
            return None
        try:
            return img.buf[o:end].decode("ascii")
        except UnicodeDecodeError:
            return None


def load_names(path):
    """class/protocol -> [selector] in method-list order, from `ipsw dyld macho --objc`.

    ipsw resolves the cache's uniqued selector table, which an extracted image does not
    contain; its listing is in method-list order, so names join to the metadata parsed here
    by position. The join is checked rather than trusted: a selector's colon count must
    equal the number of arguments in its type encoding, and a mismatch is reported."""
    names, kind, name = {}, None, None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"^@(protocol|interface) (\w+)", line)
            if m:
                kind, name = m.group(1), m.group(2)
                names.setdefault(name, [])
                continue
            if line.startswith("@end"):
                kind, name = None, None
                continue
            m = re.match(r"^-\[(\w+) (.+)\];$", line.rstrip("\n"))
            if m and name is not None and m.group(1) == name:
                names[name].append(m.group(2))
    return names


def argument_count(types):
    """How many arguments after self and _cmd the encoding describes."""
    if not types:
        return None
    n, i, depth = -3, 0, 0   # the return type, self and _cmd come first and are not arguments
    while i < len(types):
        c = types[i]
        if c in "{[(":
            if depth == 0:
                n += 1      # a struct or array passed by value is one argument
            depth += 1
        elif c in "}])":
            depth -= 1
        elif depth == 0 and c.isdigit():
            i += 1
            continue
        elif depth == 0 and c not in "rnNoORV^?":   # qualifiers, pointers, and @? (a block)
            n += 1
        i += 1
    return max(n, 0)


def load_selmap(path):
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"^(0x[0-9a-f]+):\s*(\S+)\s*$", line)
            if m:
                SELMAP[int(m.group(1), 16)] = m.group(2)


def method_list(img, addr):
    """[(selector, types, imp)] from a method_list_t, big or small entries.

    A small list (entsize bit 31) stores three int32 relative offsets per entry
    instead of three pointers. Its `name` offset points at a selector REFERENCE
    -- a pointer to the string -- unless the list has been uniqued in place, so
    both readings are tried and the one that spells a selector wins."""
    if not addr:
        return []
    entsize_flags = img.u32(addr)
    count = img.u32(addr + 4)
    if entsize_flags is None or count is None or count > 100000:
        return []
    small = bool(entsize_flags & 0x80000000)
    entsize = entsize_flags & 0xFFFC
    out = []
    for i in range(count):
        base = addr + 8 + i * (12 if small else entsize)
        if small:
            noff, toff, ioff = img.i32(base), img.i32(base + 4), img.i32(base + 8)
            if noff is None or toff is None:
                break
            sel = img.cstr(img.u64(base + noff) or 0)
            if sel is None:
                # a uniqued list points at the string itself rather than at a selector
                # reference; only accept that reading when it really starts a string,
                # because reading one byte into "...Threadgroup" yields a plausible name
                sel = img.cstr_start(base + noff)
            types = img.cstr(base + 4 + toff)
            imp = base + 8 + ioff if ioff is not None else None
        else:
            sel = img.cstr(img.u64(base) or 0)
            types = img.cstr(img.u64(base + 8) or 0)
            imp = img.u64(base + 16)
        out.append((sel, types, imp))
    return out


ABSTRACT_STUB = "_objc_msgSend$doesNotRecognizeSelector:"


def branch_target(word, addr):
    """The target of an unconditional B or BL at `addr`, or None."""
    if (word >> 26) in (0b000101, 0b100101):      # B, BL
        imm = word & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
        return addr + imm * 4
    return None


def classify_imp(img, imp, stubs=None):
    """"abstract" / "nil-stub" / "real" for one implementation, from its own bytes.

    10.1 of docs/metal.md is the reason this exists: Apple writes an abstract method as a
    real entry in the method list whose body is `[self doesNotRecognizeSelector:_cmd]`, and
    when it refuses, the exception names the RECEIVER -- so a base class's abstract method
    reads in the log exactly like a method missing from OUR subclass. A method list entry
    is not behaviour, and `respondsToSelector:` cannot tell the difference. This can: the
    body is read, and a branch to the image's `doesNotRecognizeSelector:` stub is the whole
    signature of an abstract method.

    No ipsw call per method here (there are hundreds, and `ipsw dyld disass` pays for the
    whole cache every time): B and BL are decoded in place, which is all that is needed."""
    if not imp:
        return "no imp"
    stub = stubs if stubs is not None else img.abstract_stub()
    words = []
    for i in range(10):
        w = img.u32(imp + i * 4)
        if w is None:
            break
        words.append(w)
    verdict = "real"
    for i, w in enumerate(words):
        at = imp + i * 4
        target = branch_target(w, at)
        if target is not None and stub and target == stub:
            return "abstract"
        if w == 0xD65F03C0:   # ret
            # a body of at most three instructions that returns and calls nothing is
            # Apple's silent "override me", and it is NOT an abort
            if i <= 2 and not any(branch_target(x, imp + j * 4) is not None
                                 for j, x in enumerate(words[:i])):
                verdict = "nil-stub"
            break
        if target is not None and (target < imp or target > imp + 9 * 4):
            break   # a tail call ends the body as surely as a return
    return verdict


def protocols(img):
    """name -> {required, optional, inherits} with (selector, types) lists."""
    sect = (img.sections.get("__DATA_CONST,__objc_protolist")
            or img.sections.get("__DATA,__objc_protolist"))
    if sect is None:
        return {}
    addr, size, _ = sect
    found = {}
    for i in range(size // 8):
        p = img.u64(addr + i * 8)
        if not p:
            continue
        name = img.cstr(img.u64(p + 8) or 0)
        if name is None:
            continue
        inherits = []
        plist = img.u64(p + 16)
        if plist:
            n = img.u64(plist) or 0
            for j in range(min(n, 64)):
                sup = img.u64(plist + 8 + j * 8)
                if sup:
                    sname = img.cstr(img.u64(sup + 8) or 0)
                    if sname:
                        inherits.append(sname)
        found[name] = {
            "inherits": inherits,
            "required": method_list(img, img.u64(p + 24) or 0),
            "optional": method_list(img, img.u64(p + 40) or 0),
        }
    return found


def classes(img):
    """name -> {super, methods:[(sel, types)], protocols:[name]} for this image.

    The superclass of a class whose parent lives in another image cannot be read
    here (the pointer is bound at load time), so `super` may be None; the chain
    is reassembled by whoever has all the images."""
    sect = (img.sections.get("__DATA_CONST,__objc_classlist")
            or img.sections.get("__DATA,__objc_classlist"))
    if sect is None:
        return {}
    addr, size, _ = sect
    out = {}
    for i in range(size // 8):
        cls = img.u64(addr + i * 8)
        if not cls:
            continue
        superp = img.u64(cls + 8)
        data = img.u64(cls + 32)
        if not data:
            continue
        data &= ~7
        name = img.cstr(img.u64(data + 24) or 0)
        if name is None:
            continue
        protos = []
        plist = img.u64(data + 40)
        if plist:
            n = img.u64(plist) or 0
            for j in range(min(n, 64)):
                pr = img.u64(plist + 8 + j * 8)
                if pr:
                    pn = img.cstr(img.u64(pr + 8) or 0)
                    if pn:
                        protos.append(pn)
        supername = None
        if superp:
            sdata = img.u64(superp + 32)
            if sdata:
                supername = img.cstr(img.u64((sdata & ~7) + 24) or 0)
        out[name] = {
            "super": supername,
            "protocols": protos,
            "methods": method_list(img, img.u64(data + 32) or 0),
        }
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("macho")
    ap.add_argument("--proto", action="append", default=[])
    ap.add_argument("--cls", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--verdicts", action="store_true",
                    help="for --cls, say whether each body is abstract, a nil-stub or real")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--with", dest="neighbours", action="append", default=[],
                    help="another extracted image whose strings this one points into")
    ap.add_argument("--selmap", action="append", default=[],
                    help="output of `ipsw dyld objc sel <DSC> -i <image>`")
    args = ap.parse_args(argv)

    for path in args.selmap:
        load_selmap(path)
    img = Image(args.macho)
    img.link([Image(n) for n in args.neighbours])
    protos, clss = protocols(img), classes(img)

    if args.list:
        for name in sorted(protos):
            p = protos[name]
            print("%-48s %3d required %3d optional  %s"
                  % (name, len(p["required"]), len(p["optional"]),
                     ", ".join(p["inherits"])))
        return 0

    want_p = {n: protos[n] for n in args.proto if n in protos}
    want_c = {n: clss[n] for n in args.cls if n in clss}
    for n in args.proto:
        if n not in protos:
            print("protometh: %s is not defined in this image" % n, file=sys.stderr)
    for n in args.cls:
        if n not in clss:
            print("protometh: class %s is not in this image" % n, file=sys.stderr)

    if args.json:
        json.dump({"protocols": want_p, "classes": want_c}, sys.stdout, indent=1)
        print()
        return 0

    for name, p in want_p.items():
        print("@protocol %s <%s>" % (name, ", ".join(p["inherits"])))
        for kind in ("required", "optional"):
            for sel, types, _ in p[kind]:
                print("  %-9s %-60s %s" % (kind, sel, types))
    for name, c in want_c.items():
        print("@interface %s : %s <%s>"
              % (name, c["super"], ", ".join(c["protocols"])))
        for sel, types, imp in c["methods"]:
            verdict = classify_imp(img, imp) if args.verdicts else ""
            print("  %-60s %-28s %-10s %s"
                  % (sel, types, verdict, "%#x" % imp if imp else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
