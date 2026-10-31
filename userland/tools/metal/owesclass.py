#!/usr/bin/env python3
"""What every class of this driver owes its Apple COUNTERPART, and who sends it.

owes.py counts what the PROTOCOLS require, and across 865 selectors it reported
0 unanswered. It was not wrong; it was answering a narrower question. The window
server died 78 times in one boot on

    -[RedwoodMetalCommandBuffer setProtectionOptions:]: unrecognized selector

and that selector is in no protocol. It is declared on a CLASS --
IOGPUMetalCommandBuffer, whose own _protectionOptions ivar it is -- and our
command buffer descends from _MTLCommandBuffer rather than from IOGPU's, so
nothing in our chain answers it and no protocol list says so.

Every selector Apple's counterpart class answers is a selector a caller may
send, because our object stands exactly where Apple's would and callers inside
Metal.framework, QuartzCore and the window server were compiled against Apple's.
The protocols are a subset of that list. So this counts the other axis:

    union(methods of the Apple counterpart's whole chain)
  - what our .m implements
  - what the Apple base classes in OUR chain really implement
  = what a caller can abort on.

"really" is the word that does the work: owes.py's base_verdicts reads each base
entry's body out of the extracted image, and an entry whose body is
[self doesNotRecognizeSelector:_cmd] is not an answer -- respondsToSelector:
says yes to it and the abort names OUR class. Those are counted as absent here,
which is what they are.

A long list of absences is not work, though. What makes it work is the second
half: for each absence, whether anything this port actually RUNS sends it.

  * QuartzCore and the window server are the same binary's two halves and
    matter most: CA::WindowServer::IOMFBServer::thread_body has no exception
    handler, so an unrecognised selector there is terminate(), not a log line.
  * then SpringBoard, then a third-party application (Resident Evil 4).

Senders are read out of the instruction stream, in the two forms the compilers
in play actually emit, and every site is classified by the ABI register the
selector lands in -- which is the whole fatal/harmless distinction:

    bl _objc_msgSend$foo             the selector is x1 inside the stub: A SEND
    adrp/ldr selref(foo) -> x1       the selector is the message:       A SEND
    adrp/ldr selref(foo) -> x2+      the selector is an ARGUMENT, and the
                                     message in x1 at that call site says to
                                     what: respondsToSelector: is harmless,
                                     performSelector: is not.

Two independent references are required before a selector is called sent:
a __objc_selrefs slot or an __objc_stubs entry (the compiler emits neither for
a selector it never messages) AND an instruction in __text that reaches it.
Either one alone is reported as such and not counted.

The method is checked against the boot that prompted it, which is the only test
it could fail. Run with --src pointing at redwoodmetal.m as it stood before
7fdc217 -- the source that was on the phone at 01:51 -- it names

    setProtectionOptions:  fatal  ours  QuartzCore: 10 message
          objc_stub 0x18830223c  CA::OGL::MetalContext::start_command_buffer(...)
    protectionOptions      fatal  ours  QuartzCore: 13 message

at the top of RedwoodMetalCommandBuffer's 68, and `ipsw dyld disass -q -a
0x188302228 -c 10` shows the pair in one place: a read of -protectionOptions,
a compare, and a -setProtectionOptions: if they differ. Run against the source
as it is now it names neither, because both are answered.

What this CANNOT see, and must not be read as seeing:

  * a send through a SEL computed at runtime, through sel_registerName on a
    string built at runtime, or from a library loaded later. A zero here means
    "no send site this tool can see in this binary", never "never sent";
  * which code path a site is on. A site is somewhere in the binary; whether
    the window server reaches it on the path this port takes is a separate
    question that only a boot answers;
  * for Resident Evil 4, appsends.py's own floor applies unchanged: 1 167 of
    its 2 972 objc_msgSend calls are not traceable to a selector global;
  * a selector Apple's counterpart answers only because its own ivars exist.
    Those are listed, because a caller can still send them, but the right
    answer to several of them is a refusal and not an implementation;
  * WHICH OBJECT a site sends to. --name-sites gives the sending function's
    name, and the "reaches" column reads an answer off it, but that reading is
    an argument and not a measurement: -[MTLIOAccelTexture dealloc] sending
    -resourceSize cannot be our texture, and a free function in QuartzCore
    probably is, and neither is proved by the instruction. The column says
    "maybe" wherever it does not know, and "maybe" is not "no";
  * a homonym. `validate`, `uniqueIdentifier` and `formattedDescription:` are
    declared by Apple's counterpart AND by a hundred unrelated classes, so
    SpringBoard's 332 sends of -uniqueIdentifier are mostly to its own
    objects. The sending function's name is the only thing that separates
    those, which is why it is printed next to the count and not instead of it.

usage:
  C=ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64
  for i in /System/Library/Frameworks/Metal.framework/Metal \
           /System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11 \
           /System/Library/PrivateFrameworks/IOGPU.framework/IOGPU \
           /System/Library/Frameworks/QuartzCore.framework/QuartzCore; do
          ipsw dyld macho $C $i --objc    > $(basename $i).objc.txt
          ipsw dyld objc sel $C -i $i    >> selmap.txt
          ipsw dyld extract $C $i --output ext
  done
  owesclass.py --dump Metal.objc.txt --dump AGXMetalA11.objc.txt \
               --dump IOGPU.objc.txt --dump QuartzCore.objc.txt \
               --images ext --selmap selmap.txt \
               --sender QuartzCore=ext/QuartzCore \
               --sender backboardd=ios165/rootfs/usr/libexec/backboardd \
               --sender SpringBoard=.../SpringBoard.app/SpringBoard \
               --sender RE4='.../Resident Evil 4' \
               [--sent] [--verbose] [--class RedwoodMetalCommandBuffer]
"""

import argparse
import collections
import os
import re
import struct
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import owes

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# appsends.py is a program, and importing it would run its argparse. Its Mach-O
# reader, its arm64 decoders and its metal-cpp table are exactly what is needed
# here, so it is loaded as a module with its entry point intact but unrun.
_APPSENDS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "appsends.py")
appsends = type(sys)("appsends")
appsends.__dict__["__name__"] = "appsends.module"
appsends.__dict__["np"] = np
exec(compile(open(_APPSENDS).read(), _APPSENDS, "exec"), appsends.__dict__)

# What the root class answers, so that a counterpart's chain does not appear to
# owe its own inherited Foundation behaviour. This is the one list here that is
# not read out of the cache: NSObject lives in libobjc and Foundation, neither
# of which is dumped, and every one of these is answered by every object that
# descends from NSObject -- which ours do. .cxx_destruct is on it because the
# compiler emits it and no caller sends it.
NSOBJECT_ANSWERS = {
    "init", "dealloc", "finalize", "class", "superclass", "self", "isa",
    "hash", "isEqual:", "isKindOfClass:", "isMemberOfClass:", "isProxy",
    "respondsToSelector:", "conformsToProtocol:",
    "retain", "release", "autorelease", "retainCount", "zone",
    "description", "debugDescription",
    "performSelector:", "performSelector:withObject:",
    "performSelector:withObject:withObject:",
    "methodForSelector:", "methodSignatureForSelector:",
    "forwardingTargetForSelector:", "forwardInvocation:",
    "doesNotRecognizeSelector:", "copy", "mutableCopy",
    "allowsWeakReference", "retainWeakReference",
    "_tryRetain", "_isDeallocating", ".cxx_destruct",
}

# A selector sent with another selector as its argument. Which of these the
# caller used is the difference between a crash and a branch not taken, so the
# verdict is attached to the name rather than to the count.
PROBES = {
    "respondsToSelector:": "harmless",
    "instancesRespondToSelector:": "harmless",
    "conformsToProtocol:": "harmless",
    "methodSignatureForSelector:": "harmless",
    "instanceMethodSignatureForSelector:": "harmless",
    "methodForSelector:": "harmless",
    "instanceMethodForSelector:": "harmless",
    "performSelector:": "fatal",
    "performSelector:withObject:": "fatal",
    "performSelector:withObject:withObject:": "fatal",
    "performSelector:withObject:afterDelay:": "fatal",
    "performSelectorOnMainThread:withObject:waitUntilDone:": "fatal",
    "addObserver:selector:name:object:": "deferred",
    "addTarget:action:": "deferred",
}

# The runtime functions that take a selector in a register, and what a selector
# reaching one of them means. objc_opt_respondsToSelector is here because it is
# how clang has compiled -respondsToSelector: since iOS 13, and it takes the
# probed selector in x1 -- exactly where objc_msgSend takes the message. Reading
# the register without reading the callee would call every probe a send.
HELPERS = {
    "objc_msgSend": "message",
    "objc_msgSendSuper2": "message",
    "objc_opt_respondsToSelector": "harmless",
    "sel_isEqual": "harmless",
    "sel_getName": "harmless",
    "class_respondsToSelector": "harmless",
    "class_getInstanceMethod": "harmless",
    "class_getMethodImplementation": "harmless",
    "method_getName": "harmless",
    "objc_opt_isKindOfClass": "harmless",
}


# --------------------------------------------------------------------------
# the class side: what the counterpart's chain answers that ours does not
# --------------------------------------------------------------------------

def counterpart_name(spec):
    """The Apple class name out of owes.CLASSES' evidence string.

    The string is written for a reader -- "AGXA11FamilyBuffer : AGXBuffer
    <MTLBuffer> (its resource half is ...)" -- and only its first word is a
    class. The rest of the chain is NOT taken from it: it is walked out of the
    dumps below, because the note is prose and the dump is the cache. That
    matters: the note says AGXA11FamilyTexture descends from IOGPUMetalTexture,
    and the cache says it descends from AGXTexture, which descends from
    IOGPUMetalTexture -- so the note is one class short and the walk is not.
    """
    return re.match(r"\s*(\w+)", spec["apple"]).group(1)


def chain(clss, start, stop=("NSObject",)):
    """start and every superclass of it the dumps know, nearest first."""
    out, seen, cur = [], set(), start
    while cur and cur not in seen and cur not in stop:
        seen.add(cur)
        out.append(cur)
        entry = clss.get(cur)
        if entry is None:
            break
        cur = entry["super"]
    return out


def real_methods(clss, verdicts, classes):
    """Selectors these classes answer with a BODY, and the abstract ones apart.

    An entry whose body is [self doesNotRecognizeSelector:_cmd] is not an
    answer: respondsToSelector: says yes and the abort names the subclass, so
    counting it as answered is how 28 of these came to sit under this driver
    unnoticed (docs/metal.md 10.1). A class whose bodies could not be read at
    all is returned in the third slot, so the caller can say so instead of
    quietly counting its entries as real.
    """
    answered, abstract, unread = set(), {}, []
    for c in classes:
        entry = clss.get(c)
        if entry is None:
            continue
        sels = entry["methods"]
        if verdicts and sels and not any((c, s) in verdicts for s in sels):
            unread.append(c)
        for s in sels:
            verdict = verdicts.get((c, s), ("", ""))[0]
            if verdict == "abstract":
                abstract.setdefault(s, c)
            else:
                answered.add(s)
    return answered - set(abstract), abstract, unread


# --------------------------------------------------------------------------
# the caller side: send sites, in a Mach-O, by selector
# --------------------------------------------------------------------------

def load_selmaps(paths):
    sm = {}
    for p in paths:
        with open(p, errors="replace") as f:
            for line in f:
                m = re.match(r"^(0x[0-9a-fA-F]+):\s*(\S.*?)\s*$", line)
                if m:
                    sm[int(m.group(1), 16)] = m.group(2)
    return sm


SEL_RE = re.compile(r"^[A-Za-z_.][A-Za-z0-9_.:]{0,199}$")


class Sends:
    """Every Objective-C send site in one Mach-O, indexed by selector.

    Two sources are read and kept apart, because they are two independent
    references and collapsing them would leave one:

      * the DATA side -- a __objc_selrefs slot, or a __objc_stubs entry. The
        compiler emits neither for a selector the translation unit never
        messages, so the slot alone already says more than a string does;
      * the TEXT side -- an instruction that reaches that slot or that stub.

    A selector with a slot and no instruction is reported as such, never as
    sent: that is what a selector referenced only by a method list looks like.
    """

    def __init__(self, path, name=None, selmap=None):
        self.name = name or os.path.basename(path)
        self.macho = appsends.MachO(path)
        self.text = appsends.Text(self.macho)
        self.selmap = selmap or {}
        self.notes = []
        self._readable = [(a, sz, fo) for (a, sz, fo, _fl, _r1)
                          in self.macho.sections.values() if fo]
        self.base = min((a for a, _, _ in self._readable), default=0) & ~0xFFFFFFF
        self.selrefs = self._read_selrefs()
        self.slot_of = {}
        for a, s in self.selrefs.items():
            self.slot_of.setdefault(s, []).append(a)
        self.objc_stubs, self.msgsend_got = self._read_objc_stubs()
        self.stub_of = {}
        for a, s in self.objc_stubs.items():
            self.stub_of.setdefault(s, []).append(a)
        self.plain_msgsend = self._find_plain_msgsend()
        self.helpers, self.text_stubs = self._read_helpers()
        self.bl_to = self._read_bl()
        self.literal_loads = self._read_literal_loads()
        self.cpp_table, _span = appsends.registration_table(
            self.macho, self.text, self.macho.stub_names())
        if self.cpp_table:
            self.notes.append("%d selectors in a metal-cpp sel_registerName table"
                              % len(self.cpp_table))

    # ---- data side ----

    def cstr(self, va):
        for a, sz, fo in self._readable:
            if a <= va < a + sz:
                blob = self.macho.read(fo + (va - a), 256)
                end = blob.find(b"\0")
                if end <= 0:
                    return None
                try:
                    return blob[:end].decode("ascii")
                except UnicodeDecodeError:
                    return None
        return None

    def resolve(self, raw):
        """A chained-fixup selector pointer -> the selector it names, or None.

        Three candidates, in order, and the first that reads back as a
        plausible selector wins. A cache image's fixup carries the whole cache
        address in its low 40 bits; an on-disk binary's carries an offset from
        the image base in its low 36. Both shapes turn up here -- QuartzCore
        comes out of the cache, backboardd off the filesystem -- and guessing
        wrong gives an empty table rather than a wrong one, which is why the
        resolution counts are printed.
        """
        for va in (raw & 0xFFFFFFFFFF, self.base + (raw & 0xFFFFFFFFF), raw):
            s = self.cstr(va)
            if s and SEL_RE.match(s):
                return s
            s = self.selmap.get(va)
            if s:
                return s
        return None

    def _read_selrefs(self):
        out, raw_total, unresolved = {}, 0, 0
        for key in ("__DATA_CONST,__objc_selrefs", "__DATA,__objc_selrefs",
                    "__AUTH_CONST,__objc_selrefs"):
            if key not in self.macho.sections:
                continue
            a, sz, fo, _fl, _r1 = self.macho.sections[key]
            if not fo or sz < 8:
                continue
            vals = struct.unpack_from("<%dQ" % (sz // 8), self.macho.read(fo, sz), 0)
            for i, v in enumerate(vals):
                raw_total += 1
                s = self.resolve(v)
                if s is None:
                    unresolved += 1
                else:
                    out[a + i * 8] = s
        self.notes.append("%d selector-reference slots, %d resolved to a name"
                          % (raw_total, raw_total - unresolved))
        return out

    def _read_objc_stubs(self):
        """{stub address: selector} and the GOT slot every stub branches through.

        A stub is `adrp x1,p; ldr x1,[x1,#o]; adrp x16,q; ldr x16,[x16,#n];
        br x16`, so the selector is whichever selref slot its first two
        instructions address -- which is how the stub is IDENTIFIED rather than
        assumed, since an extracted cache image has no symbol for it. The GOT
        slot its third and fourth address is objc_msgSend's, and finding the
        plain __stubs entry that uses the same slot is how a call to
        objc_msgSend is recognised below.
        """
        out, gots = {}, collections.Counter()
        key = "__TEXT,__objc_stubs"
        if key not in self.macho.sections or not self.macho.sections[key][2]:
            self.notes.append("no __objc_stubs: the older convention, where every send "
                              "loads its own selector")
            return out, None
        a, sz, fo, _fl, _r1 = self.macho.sections[key]
        words = struct.unpack_from("<%dI" % (sz // 4), self.macho.read(fo, sz & ~3), 0)
        for i in range(len(words) - 4):
            w0, w1 = words[i], words[i + 1]
            if not appsends.is_adrp(w0) or (w0 & 31) != 1:
                continue
            ok, imm, rn, rd = appsends.ldr_u64(w1)
            if not (ok and rn == 1 and rd == 1):
                continue
            pc = a + i * 4
            slot = appsends.adrp_page(w0, pc) + imm
            sel = self.selrefs.get(slot)
            if sel is None:
                continue
            out[pc] = sel
            w2, w3 = words[i + 2], words[i + 3]
            if appsends.is_adrp(w2):
                ok2, imm2, rn2, _ = appsends.ldr_u64(w3)
                if ok2 and rn2 == (w2 & 31):
                    gots[appsends.adrp_page(w2, pc + 8) + imm2] += 1
        self.notes.append("%d __objc_stubs entries (bl _objc_msgSend$sel)" % len(out))
        return out, (gots.most_common(1)[0][0] if gots else None)

    def _find_plain_msgsend(self):
        """The __TEXT,__stubs entries that are objc_msgSend itself.

        Named by symbol where the image has an indirect symbol table, and
        otherwise by the GOT slot the __objc_stubs all share -- a cache image
        has no symbols left, so without the second route every argument-position
        selector in QuartzCore would read as 'guard unknown'.
        """
        out = set()
        for addr, sym in self.macho.stub_names().items():
            if sym.lstrip("_") in ("objc_msgSend", "objc_msgSendSuper2"):
                out.add(addr)
        if out:
            self.notes.append("%d __stubs entries named objc_msgSend by symbol"
                              % len(out))
            return out
        if self.msgsend_got is None or "__TEXT,__stubs" not in self.macho.sections:
            return out
        a, sz, fo, _fl, _r1 = self.macho.sections["__TEXT,__stubs"]
        words = struct.unpack_from("<%dI" % (sz // 4), self.macho.read(fo, sz & ~3), 0)
        for i in range(len(words) - 1):
            w0, w1 = words[i], words[i + 1]
            if not appsends.is_adrp(w0):
                continue
            ok, imm, rn, _ = appsends.ldr_u64(w1)
            if ok and rn == (w0 & 31):
                pc = a + i * 4
                if appsends.adrp_page(w0, pc) + imm == self.msgsend_got:
                    out.add(pc)
        self.notes.append("%d __stubs entries reach objc_msgSend's GOT slot %#x"
                          % (len(out), self.msgsend_got))
        return out

    def _read_helpers(self):
        """({stub address: runtime function it calls}, every __stubs address).

        Only the runtime entry points that TAKE a selector are named, because
        those are the ones that change a site's meaning. The second set is
        every __stubs address, so that a call into one this tool cannot name is
        reported as unnamed instead of being silently read as a send.
        """
        helpers, allstubs = {}, set()
        for addr, sym in self.macho.stub_names().items():
            allstubs.add(addr)
            if sym.lstrip("_") in HELPERS:
                helpers[addr] = sym
        if "__TEXT,__stubs" in self.macho.sections:
            a, sz, _fo, _fl, _r1 = self.macho.sections["__TEXT,__stubs"]
            allstubs |= {a + k for k in range(0, sz, 12)}
        if helpers:
            self.notes.append("%d __stubs entries are selector-taking runtime "
                              "functions" % len(helpers))
        return helpers, allstubs

    # ---- text side ----

    def _read_bl(self):
        """{objc stub address: the pcs that call it}.

        Only calls to a stub this image has are kept. Keeping every bl would be
        a dictionary of millions of entries in a 280 MB application, for a
        question that is only ever asked about the stubs.
        """
        wanted = np.array(sorted(set(self.objc_stubs) | self.plain_msgsend),
                          dtype=np.int64)
        if not len(wanted):
            return {}
        code = self.text.code
        idx = np.flatnonzero((code & np.uint32(0xFC000000)) == np.uint32(0x94000000))
        imm = (code[idx] & np.uint32(0x3FFFFFF)).astype(np.int64)
        imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
        pcs = self.text.va + 4 * idx.astype(np.int64)
        tgt = pcs + imm * 4
        keep = np.isin(tgt, wanted)
        out = {}
        for p, t in zip(pcs[keep].tolist(), tgt[keep].tolist()):
            out.setdefault(t, []).append(p)
        self.notes.append("%d calls to those stubs in __text" % int(keep.sum()))
        return out

    def _read_literal_loads(self):
        """{selector-reference slot: [(pc, index, register)]} for `ldr xT, <label>`.

        The third form, and the one that would have made backboardd read as
        sending nothing. An adrp is not the only way to name a slot: where
        __text and __objc_selrefs are within a megabyte of each other -- true
        of a 1 MB daemon, never of a framework in the shared cache -- clang
        materialises a SEL with a PC-relative literal load instead, and it is
        the form every one of backboardd's 14 objc_opt_respondsToSelector
        probes uses. Decoding only the adrp forms found three selector loads in
        backboardd; this finds the rest.
        """
        if not self.selrefs:
            return {}
        lo, hi = min(self.selrefs), max(self.selrefs) + 8
        code = self.text.code
        idx = np.flatnonzero((code & np.uint32(0xFF000000)) == np.uint32(0x58000000))
        if not len(idx):
            # said rather than skipped: Resident Evil 4 has not one integer
            # ldr-literal in 50 381 725 instructions, and a silent zero here
            # would read as a form that had not been looked for
            self.notes.append("no ldr-literal form in __text at all")
            return {}
        w = code[idx].astype(np.int64)
        imm = (w >> 5) & 0x7FFFF
        imm = np.where(imm & (1 << 18), imm - (1 << 19), imm)
        pcs = self.text.va + 4 * idx.astype(np.int64)
        tgt = pcs + imm * 4
        keep = (tgt >= lo) & (tgt < hi)
        out = {}
        for i, pc, t, rt in zip(idx[keep].tolist(), pcs[keep].tolist(),
                                tgt[keep].tolist(), (w[keep] & 31).tolist()):
            if t in self.selrefs:
                out.setdefault(t, []).append((pc, i, rt))
        self.notes.append("%d ldr-literal selector loads" % sum(map(len, out.values())))
        return out

    def _window(self, i, back=16, fwd=12):
        """What each register holds at the FIRST call at or after instruction i.

        Only the forms a selector reference takes are decoded; anything that
        writes a register drops it, which is the same rule appsends.py had to
        add after `add x20,x20,#0xd18` was read as seventeen sends of
        -isLowPower. The simulation stops at that first call and not at the end
        of the window: a second call further on would overwrite x1 and the
        guard would then be named after the wrong message.
        """
        lo = max(0, i - back)
        hi = min(self.text.n, i + fwd)
        addr, sel, bls = {}, {}, []
        for j in range(lo, hi):
            w = int(self.text.code[j])
            pc = self.text.pc(j)
            if appsends.is_adrp(w):
                addr[w & 31] = appsends.adrp_page(w, pc)
                sel.pop(w & 31, None)
                continue
            ok, imm, rn, rd = appsends.add_imm64(w)
            if ok and rn in addr:
                addr[rd] = addr[rn] + imm
                sel.pop(rd, None)
                continue
            ok, imm, rn, rd = appsends.ldr_u64(w)
            if ok:
                name = self.selrefs.get(addr.get(rn, -1) + imm) if rn in addr else None
                addr.pop(rd, None)
                if name:
                    sel[rd] = name
                else:
                    sel.pop(rd, None)
                continue
            ok, imm, rn, rt = appsends.ldp_64(w)
            if ok and rn in addr:
                rt2 = (w >> 10) & 31
                for k, off in ((rt, 0), (rt2, 8)):
                    name = self.selrefs.get(addr[rn] + imm + off)
                    addr.pop(k, None)
                    if name:
                        sel[k] = name
                    else:
                        sel.pop(k, None)
                continue
            if (w & 0xFC000000) == 0x94000000:
                bls.append((pc, j))
                imm = w & 0x3FFFFFF
                if imm & (1 << 25):
                    imm -= 1 << 26
                stub = pc + imm * 4
                if stub in self.objc_stubs:
                    sel[1] = self.objc_stubs[stub]
                if j >= i:
                    return sel, bls
                # a call before the load clobbers x0-x17; x19-x28 survive it
                for r in list(addr):
                    if r <= 17:
                        addr.pop(r)
                for r in list(sel):
                    if r <= 17:
                        sel.pop(r)
                continue
            rd = appsends.writes_reg(w)
            if rd is not None:
                addr.pop(rd, None)
                sel.pop(rd, None)
        return sel, bls

    def _loads_of(self, slot):
        """(pc, index, destination register) for every load of this slot."""
        out = []
        off = slot & 0xFFF
        for k in self.text.by_page.get(slot & ~0xFFF, []):
            i, r = int(self.text.adrp_idx[k]), int(self.text.adrp_rd[k])
            for j in range(i + 1, min(i + 13, self.text.n)):
                w = int(self.text.code[j])
                ok, imm, rn, rd = appsends.ldr_u64(w)
                if ok and rn == r and imm == off:
                    out.append((self.text.pc(j), j, rd))
                    break
                ok, imm, rn, rt = appsends.ldp_64(w)
                if ok and rn == r and imm in (off, off - 8):
                    out.append((self.text.pc(j), j,
                                rt if imm == off else (w >> 10) & 31))
                    break
                ok, imm, rn, rd2 = appsends.add_imm64(w)
                if ok and rn == r:
                    base = (slot & ~0xFFF) + imm
                    for j2 in range(j + 1, min(j + 13, self.text.n)):
                        w2 = int(self.text.code[j2])
                        ok2, imm2, rn2, rd3 = appsends.ldr_u64(w2)
                        if ok2 and rn2 == rd2 and base + imm2 == slot:
                            out.append((self.text.pc(j2), j2, rd3))
                            break
                        if appsends.writes_reg(w2) == rd2:
                            break
                    break
                if appsends.writes_reg(w) == r:
                    break
        return out

    def callee(self, i, fwd=10):
        """(kind, name, address) of the first call at or after instruction i.

        The callee is what decides whether a selector in a register is being
        SENT or merely handed over. x1 is the selector argument of
        objc_msgSend and equally of objc_opt_respondsToSelector, and those two
        are a crash and a branch not taken -- so a site is never classified
        from the register alone.
        """
        for j in range(i, min(i + fwd, self.text.n)):
            w = int(self.text.code[j])
            if (w & 0xFC000000) not in (0x94000000, 0x14000000):
                continue
            imm = w & 0x3FFFFFF
            if imm & (1 << 25):
                imm -= 1 << 26
            t = self.text.pc(j) + imm * 4
            if t in self.objc_stubs:
                return "objc_stub", self.objc_stubs[t], t
            if t in self.plain_msgsend:
                return "msgsend", "objc_msgSend", t
            if t in self.helpers:
                return "helper", self.helpers[t], t
            if t in self.text_stubs:
                return "unnamed", None, t
            continue
        return None, None, None

    def _role(self, sel, i, rd):
        kind, who, _t = self.callee(i)
        if kind == "objc_stub":
            return "message" if who == sel else "argument:%s" % who
        if kind == "msgsend":
            if rd == 1:
                return "message"
            regs, _ = self._window(i)
            return "argument:%s" % (regs.get(1) or "?")
        if kind == "helper":
            return "probe:%s" % who.lstrip("_")
        return "unnamed-callee:x%d" % rd if kind == "unnamed" else "no-call:x%d" % rd

    def sites(self, sel):
        """Every site at which `sel` reaches a register, classified.

        Each is a dict: form (how the selector got there), role (what the
        callee does with it), pc, via (the data-side reference, which is the
        second of the two independent references this port requires).
        """
        out = []
        for stub in self.stub_of.get(sel, []):
            for pc in self.bl_to.get(stub, []):
                out.append({"form": "objc_stub", "role": "message", "pc": pc,
                            "via": "bl %#x" % stub})
        for slot in self.slot_of.get(sel, []):
            for pc, i, rd in self._loads_of(slot):
                out.append({"form": "adrp+ldr->x%d" % rd, "role": self._role(sel, i, rd),
                            "pc": pc, "via": "slot %#x" % slot})
            for pc, i, rd in self.literal_loads.get(slot, []):
                out.append({"form": "ldr-literal->x%d" % rd,
                            "role": self._role(sel, i, rd),
                            "pc": pc, "via": "slot %#x" % slot})
        for pc in sorted(self._cpp_sites(sel)):
            out.append({"form": "metal-cpp", "role": "message", "pc": pc,
                        "via": "sel_registerName table"})
        return out

    def _cpp_sites(self, sel):
        if sel not in self.cpp_table:
            return set()
        return self.text.loads_of(self.cpp_table[sel][0])

    def knows(self, sel):
        """True when the DATA side alone references this selector."""
        return sel in self.slot_of or sel in self.stub_of or sel in self.cpp_table


class Namer:
    """The function a send site is in, out of `ipsw dyld a2s`.

    This is the difference between "the selector is sent somewhere in this
    binary" and "the selector is sent to an object of OURS", and nothing else
    here can tell those apart. -[IOGPUMetalCommandBuffer commit] sending
    -beginSegment: to self can never reach us, because our command buffer is
    not an IOGPUMetalCommandBuffer; CA::OGL::MetalContext::start_command_buffer
    sending -setProtectionOptions: reaches us every frame, because the buffer
    it sends to is the one our queue just handed it. The caller's name is what
    says which, so it is the third reference and not a decoration.

    Only addresses in the shared cache can be named: backboardd and the game
    are stripped of everything but __mh_execute_header, so a site in either is
    reported with its address and no name.
    """

    def __init__(self, cache, limit=400):
        self.cache = cache
        self.limit = limit
        self.memo = {}
        self.spent = 0

    def __call__(self, pc):
        if pc in self.memo:
            return self.memo[pc]
        if self.cache is None or self.spent >= self.limit:
            return None
        self.spent += 1
        try:
            out = subprocess.run(["ipsw", "dyld", "a2s", self.cache, hex(pc)],
                                 capture_output=True, text=True, timeout=180)
        except (OSError, subprocess.TimeoutExpired):
            self.memo[pc] = None
            return None
        # `-[Class selector:]` has a space in it, so the name is everything up to
        # the `+ offset` and not the first word
        m = re.search(r"^0x[0-9a-fA-F]+:\s*(.+?)(?:\s*\+\s*\d+)?\s*$",
                      out.stdout + out.stderr, re.M)
        name = m.group(1) if m else None
        if name and name.startswith("__Z"):
            try:
                name = subprocess.run(["c++filt", "--strip-underscore"],
                                      input=name, capture_output=True, text=True,
                                      timeout=30).stdout.strip() or name
            except (OSError, subprocess.TimeoutExpired):
                pass
        self.memo[pc] = name
        return name


def reach(owner, our_bases, apple_chain):
    """Can a send from this function land on one of OUR objects?

    The third question, and the one that turns 72 abort-capable selectors into
    the handful worth writing. Read off the sending method's owner:

      * a C++ or free function -- CA::OGL::MetalContext::start_command_buffer --
        holds an id it was handed by Metal, and the object Metal hands it is
        ours. Those reach us;
      * -[_MTLCommandBuffer ...] is a method OUR class inherits, so its self IS
        our object. Those reach us too, and are the easiest to miss;
      * -[IOGPUMetalCommandBuffer ...] or -[MTLIOAccelTexture ...] is a method
        of a class ours is not and does not inherit from. That code never runs
        with our object as self, and reaches us only if someone hands our
        object to it as an argument -- which is a different claim, and is not
        made here;
      * any other class is left undecided, because it may hold one of ours and
        may equally have a selector of the same name of its own.
    """
    if owner is None:
        return "?"
    m = re.match(r"[-+]\[(\w+)\s", owner)
    if m is None:
        return "ours"
    cls = m.group(1)
    if cls in our_bases:
        return "ours (self)"
    if cls in apple_chain:
        return "Apple's"
    if cls.startswith(("IOGPUMetal", "IOGPU", "AGX", "MTLIOAccel", "_MTLIOAccel")):
        return "Apple's"
    return "maybe"


def verdict(sites):
    """"fatal" / "guarded" / "unclear" out of a selector's site list.

    An unanswered selector sent as the MESSAGE raises, and in a thread whose
    body has no handler -- CA::WindowServer::IOMFBServer::thread_body -- that
    is terminate(). The same selector handed to respondsToSelector: is a branch
    not taken. Nothing else in this report matters as much as keeping those two
    apart, so one message-position site outweighs any number of probes, and a
    site whose callee could not be named is never counted as either.
    """
    roles = [s["role"] for s in sites]
    if "message" in roles:
        return "fatal"
    heads = {r.split(":", 1)[0] for r in roles}
    probes = {r.split(":", 1)[1] for r in roles if r.startswith("argument:")}
    if any(PROBES.get(p) == "fatal" for p in probes):
        return "fatal"
    if heads <= {"argument", "probe"} and \
            all(PROBES.get(p, "harmless") == "harmless" for p in probes if p != "?") \
            and "?" not in probes:
        return "guarded"
    return "unclear"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="append", default=[], required=True)
    ap.add_argument("--src", default=os.path.join(ROOT, "userland/src/redwoodmetal.m"))
    ap.add_argument("--images", help="directory of `ipsw dyld extract` output")
    ap.add_argument("--selmap", action="append", default=[])
    ap.add_argument("--sender", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--class", dest="only", action="append", default=[])
    ap.add_argument("--sel", action="append", default=[],
                    help="ask the senders about these selectors and nothing else")
    ap.add_argument("--sent", action="store_true", help="only the gaps something sends")
    ap.add_argument("--cache", help="the shared cache, to name each send site's function")
    ap.add_argument("--name-sites", type=int, default=0, metavar="N",
                    help="name up to N send sites with `ipsw dyld a2s` (needs --cache)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    protos, clss = owes.read_dumps(args.dump)
    mine = owes.source_methods(args.src)
    images = []
    if args.images:
        images = [os.path.join(args.images, f) for f in sorted(os.listdir(args.images))
                  if os.path.isfile(os.path.join(args.images, f))]
    verdicts, joined_badly = owes.base_verdicts(images, args.dump, args.selmap) \
        if images else ({}, 0)

    selmap = load_selmaps(args.selmap)
    namer = Namer(args.cache if args.name_sites else None, args.name_sites)
    senders = []
    for spec in args.sender:
        name, _, path = spec.partition("=")
        if not path:
            sys.exit("owesclass: --sender wants NAME=PATH, got %r" % spec)
        senders.append(Sends(path, name, selmap))
    for s in senders:
        print("sender %-14s %s" % (s.name, "; ".join(s.notes)))
    if senders:
        print()

    if args.sel:
        # the same question as below, asked of named selectors: how a claim about
        # one of them is checked without re-deriving the whole table
        for sel in args.sel:
            for s in senders:
                sites = s.sites(sel)
                if not sites:
                    print("%-52s %-12s %s" % (sel, s.name, "referenced by data only"
                          if s.knows(sel) else "no reference at all"))
                    continue
                byrole = collections.Counter(st["role"] for st in sites)
                print("%-52s %-12s %-9s %s" % (sel, s.name, verdict(sites),
                      ", ".join("%d %s" % (c, r) for r, c in byrole.most_common())))
                for st in sites[:8]:
                    print("      %-16s %-30s %-14s %s"
                          % (st["form"], st["role"], hex(st["pc"]), st["via"]))
        return 0

    names = args.only or list(owes.CLASSES)
    rows, gaps = [], {}
    for name in names:
        spec = owes.CLASSES.get(name)
        if spec is None:
            print("owesclass: %s is not in owes.py's table" % name, file=sys.stderr)
            continue
        apple = chain(clss, counterpart_name(spec))
        theirs, their_abstract, _ = real_methods(clss, verdicts, apple)
        ours_chain = [c for c in spec["bases"] if c != "NSObject"]
        base_real, base_abstract, unread = real_methods(clss, verdicts, ours_chain)
        answered = set(mine.get(name, set())) | base_real | NSOBJECT_ANSWERS
        gap = sorted(theirs - answered)
        gaps[name] = (gap, apple, ours_chain, base_abstract, unread, their_abstract)
        rows.append((name, len(theirs), len(gap)))

    print("%-30s %-44s %6s %6s %6s" %
          ("our class", "the Apple chain above it that we do not share",
           "theirs", "ours", "GAP"))
    tot_theirs = tot_gap = 0
    for name, ntheirs, ngap in rows:
        gap, apple, ours_chain, base_abstract, unread, _ = gaps[name]
        extra = [c for c in apple if c not in ours_chain and c != "NSObject"]
        print("%-30s %-44s %6d %6d %6d"
              % (name, " -> ".join(extra)[:44], ntheirs, ntheirs - ngap, ngap))
        tot_theirs += ntheirs
        tot_gap += ngap
        if unread:
            print("%-30s   the bodies of %s were not classified; its entries are "
                  "counted as real" % ("", ", ".join(unread)))
    print("%-30s %-44s %6d %6s %6d" % ("total", "", tot_theirs, "", tot_gap))
    nabs = sum(len({s for s in gaps[n][0]} & set(gaps[n][3])) for n, _, _ in rows)
    if verdicts:
        print("  %d of the %d are PRESENT in our own chain and abstract there, so "
              "respondsToSelector:" % (nabs, tot_gap))
        print("  answers yes to them and the abort names our class and not Apple's.")
    print()

    # --- who sends the gap ---
    if senders:
        print("--- of the gap, what something in this image SENDS ---")
        print("%-52s %-22s %-9s %-8s %s"
              % ("selector", "our class", "verdict", "reaches", "sender: sites"))
        counts, reached = collections.Counter(), collections.Counter()
        for name, _, _ in rows:
            gap = gaps[name][0]
            for sel in gap:
                found = []
                worst = None
                for s in senders:
                    sites = s.sites(sel)
                    if not sites:
                        if s.knows(sel):
                            found.append((s.name, "referenced, no send site", []))
                        continue
                    v = verdict(sites)
                    rank = {"fatal": 3, "unclear": 2, "guarded": 1}
                    if worst is None or rank[v] > rank[worst]:
                        worst = v
                    found.append((s.name, v, sites))
                real = [f for f in found if f[2]]
                if not real:
                    if found and not args.sent:
                        counts["referenced only"] += 1
                        print("%-52s %-22s %-9s %s" % (sel, name, "named",
                              ", ".join("%s: %s" % (n, v) for n, v, _ in found)))
                    continue
                counts[worst] += 1
                detail = []
                for n, v, sites in real:
                    byrole = collections.Counter(s["role"] for s in sites)
                    detail.append("%s: %s" % (n, ", ".join(
                        "%d %s" % (c, r) for r, c in byrole.most_common())))
                reaches = set()
                lines = []
                for n, v, sites in real:
                    for st in sites[:6] if args.verbose else sites[:1]:
                        who = namer(st["pc"])
                        r = reach(who, set(gaps[name][2]), set(gaps[name][1]))
                        reaches.add(r)
                        lines.append("      %-12s %-15s %-11s %-13s %s"
                                     % (n, st["form"], r, hex(st["pc"]),
                                        who or "(stripped)"))
                mark = "ours" if {"ours", "ours (self)"} & reaches else \
                       ("maybe" if "maybe" in reaches else
                        ("Apple's" if reaches == {"Apple's"} else "?"))
                print("%-52s %-22s %-9s %-8s %s"
                      % (sel, name, worst, mark, "; ".join(detail)))
                reached[mark] += 1
                for ln in lines:
                    print(ln)
        print()
        print("  %d of the %d gap selectors are sent as a MESSAGE somewhere and would"
              % (counts["fatal"], tot_gap))
        print("  abort; %d reach a register only to be handed to a probe that catches the"
              % counts["guarded"])
        print("  absence; %d reach one through a call this tool could not name, so they are"
              % counts["unclear"])
        print("  neither claimed nor cleared; %d are referenced by a caller's data and by"
              % counts["referenced only"])
        print("  no instruction it can see. Read the docstring before reading the rest of")
        print("  the %d as never sent." % (tot_gap - sum(counts.values())))
        if reached:
            print()
            print("  of those sent, %d are sent from code that holds or IS one of our "
                  "objects," % (reached["ours"]))
            print("  %d from a method of a class ours is not, so they reach us only if "
                  "someone" % reached["Apple's"])
            print("  hands our object to that code, and %d could not be decided (%d site "
                  "names" % (reached["maybe"], reached["?"]))
            print("  were not looked up: --name-sites was %d)." % namer.limit)

    if images:
        print()
        print("owesclass: %d (class, selector) bodies of Apple's were classified, %d left "
              "out because the name and the encoding disagreed" % (len(verdicts),
                                                                  joined_badly))
    if args.verbose:
        print()
        for name, _, _ in rows:
            gap, apple, ours_chain, base_abstract, unread, _ = gaps[name]
            print("--- %s: %d, chain %s ---" % (name, len(gap), " -> ".join(apple)))
            for sel in gap:
                where = base_abstract.get(sel)
                print("    %-60s %s" % (sel, ("ABSTRACT in our own base %s" % where)
                                        if where else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
