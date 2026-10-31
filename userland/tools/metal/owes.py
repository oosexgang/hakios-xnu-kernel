#!/usr/bin/env python3
"""What every class of this driver still owes its protocols, counted off the phone.

10.17 counted this for the device object on the device, with the runtime:
protocol_copyMethodDescriptionList + respondsToSelector:. That works for the
device because MTLDevice and MTLDeviceSPI are defined in Metal.framework, which
is loaded in every process that asks. It does NOT work for the objects below it:
MTLCommandQueueSPI, MTLCommandBufferSPI, MTLRenderCommandEncoderSPI and the rest
are defined in AGXMetalA11.bundle, which never loads on this phone -- so
objc_getProtocol returns NULL and the runtime has nothing to say.

So the sum is done here, from the cache, before the build:

  required(public protocol) + required(SPI protocol) + inherited protocols
  - what our class implements
  - what Apple's base classes in our chain implement
  = what a daemon can still abort on.

It answers three questions, and the second two are the ones that make the first
one mean anything:

  --dump          what is ABSENT from the chain: a selector no class answers, and
                  so an abort in whichever daemon sends it first;
  --images/--sel  what is PRESENT AND ABSTRACT: a base-class entry whose body is
                  [self doesNotRecognizeSelector:_cmd]. respondsToSelector: says
                  yes to those, and the abort when they are called names OUR
                  class, so they read in a log exactly like the first kind
                  (docs/metal.md 10.1). 28 of them sat under this driver;
  --asm           whether what we wrote has the SIGNATURE Apple declares, which
                  the compiler cannot check for a protocol it has never seen: a
                  method declared to return void where the caller expects a
                  struct leaves the caller's buffer uninitialised, and one
                  declared to return a struct where the caller expects a scalar
                  writes through whatever x8 held.

usage:
  C=ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64
  for i in /System/Library/Frameworks/Metal.framework/Metal \
           /System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11 \
           /System/Library/PrivateFrameworks/IOGPU.framework/IOGPU; do
          ipsw dyld macho $C $i --objc    > $(basename $i).txt   # names
          ipsw dyld objc sel $C -i $i     >> selmap.txt          # the uniqued table
          ipsw dyld extract $C $i --output ext                   # the metadata
  done
  clang -target arm64-apple-ios16.5 -isysroot ~/theos/sdks/iPhoneOS16.5.sdk -w -S \
        userland/src/redwoodmetal.m -o redwoodmetal.s
  owes.py --dump Metal.txt --dump AGXMetalA11.txt --dump IOGPU.txt \
          --images ext --selmap selmap.txt --asm redwoodmetal.s [--verbose]

The protocol lists come from the dumps, the base classes' method lists come from
the dumps, and our own come from the source: no step of it asks the runtime,
because the runtime is exactly what cannot see these protocols.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protometh

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Each of our classes, the Apple classes above it (as redwoodmetal.m declares
# them, nearest first), and the protocols its Apple counterpart declares. The
# counterpart is named so the claim can be checked: AGXA11FamilyCommandQueue
# really does say <MTLCommandQueueSPI>, and that is where this table comes from.
CLASSES = {
    "RedwoodMetalCommandQueue": {
        "bases": ["_MTLCommandQueue", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLCommandQueueSPI"],
        "apple": "AGXA11FamilyCommandQueue : IOGPUMetalCommandQueue <MTLCommandQueueSPI>",
    },
    "RedwoodMetalCommandBuffer": {
        "bases": ["_MTLCommandBuffer", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLCommandBufferSPI"],
        "apple": "AGXA11FamilyCommandBuffer : IOGPUMetalCommandBuffer <MTLCommandBufferSPI>",
    },
    "RedwoodMetalRenderEncoder": {
        "bases": ["_MTLCommandEncoder", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLRenderCommandEncoderSPI"],
        "apple": "AGXA11FamilyRenderContext : IOGPUMetalRenderCommandEncoder "
                 "<MTLRenderCommandEncoderSPI>",
    },
    "RedwoodMetalComputeEncoder": {
        "bases": ["_MTLCommandEncoder", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLComputeCommandEncoderSPI"],
        "apple": "AGXA11FamilyComputeContext : IOGPUMetalComputeCommandEncoder "
                 "<MTLComputeCommandEncoderSPI>",
    },
    "RedwoodMetalRenderPipeline": {
        "bases": ["_MTLRenderPipelineState", "NSObject"],
        "protocols": ["MTLRenderPipelineStateSPI"],
        "apple": "AGXA11FamilyRenderPipeline : _MTLRenderPipelineState "
                 "<MTLRenderPipelineStateSPI>",
    },
    "RedwoodMetalComputePipeline": {
        "bases": ["_MTLComputePipelineState", "NSObject"],
        "protocols": ["MTLComputePipelineStateSPI"],
        "apple": "AGXA11FamilyComputePipeline : _MTLComputePipelineState "
                 "<MTLComputePipelineStateSPI>",
    },
    "RedwoodMetalTexture": {
        "bases": ["_MTLResource", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLTextureSPI"],
        "apple": "AGXA11FamilyTexture : IOGPUMetalTexture <MTLTextureSPI>",
    },
    "RedwoodMetalBuffer": {
        "bases": ["_MTLResource", "_MTLObjectWithLabel", "NSObject"],
        "protocols": ["MTLBuffer", "MTLResourceSPI"],
        "apple": "AGXA11FamilyBuffer : AGXBuffer <MTLBuffer> (its resource half is "
                 "IOGPUMetalResource <MTLResourceSPI>)",
    },
    "RedwoodMetalSampler": {
        "bases": ["_MTLSamplerState", "NSObject"],
        "protocols": ["MTLSamplerStateSPI"],
        "apple": "AGXA11FamilySampler : _MTLSamplerState <MTLSamplerStateSPI>",
    },
    "RedwoodMetalDepthStencilState": {
        "bases": ["_MTLDepthStencilState", "NSObject"],
        "protocols": ["MTLDepthStencilStateSPI"],
        "apple": "AGXA11FamilyDepthStencilState : _MTLDepthStencilState "
                 "<MTLDepthStencilStateSPI>",
    },
    "RedwoodMetalDevice": {
        "bases": ["_MTLDevice", "NSObject"],
        "protocols": ["MTLDeviceSPI"],
        "apple": "AGXA11FamilyDevice : IOGPUMetalDevice <MTLDeviceSPI>",
    },
}


def read_dumps(paths):
    """(protocols, classes) out of `ipsw dyld macho --objc` text.

    Block context matters: a protocol's methods are printed in the same
    `-[Name selector];` shape as a class's, and NSObject is the name of both a
    protocol and a class, so lines are attributed to whichever block they are in
    rather than by their own spelling."""
    protos, clss = {}, {}
    for path in paths:
        kind, name, section = None, None, "required"
        with open(path, errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                m = re.match(r"^@protocol (\w+)", line)
                if m:
                    kind, name, section = "proto", m.group(1), "required"
                    protos.setdefault(name, {"required": set(), "optional": set(),
                                             "inherits": []})
                    sup = re.search(r"<([^>]*)>", line)
                    if sup:
                        protos[name]["inherits"] = [s.strip() for s in
                                                    sup.group(1).split(",") if s.strip()]
                    continue
                m = re.match(r"^@interface (\w+)", line)
                if m:
                    kind, name = "class", m.group(1)
                    sup = re.match(r"^@interface \w+ : (\w+)", line)
                    entry = clss.setdefault(name, {"super": None, "methods": set()})
                    if sup:
                        entry["super"] = sup.group(1)
                    continue
                if line.startswith("@end"):
                    kind, name = None, None
                    continue
                if line.startswith("@optional"):
                    section = "optional"
                    continue
                if line.startswith("@required"):
                    section = "required"
                    continue
                m = re.match(r"^-\[(\w+) (.+)\];$", line)
                if m and name is not None and m.group(1) == name:
                    if kind == "proto":
                        protos[name][section].add(m.group(2))
                    else:
                        clss[name]["methods"].add(m.group(2))
    return protos, clss


def required_of(protos, names, seen=None):
    """Every required selector of these protocols and of the protocols they adopt."""
    seen = seen if seen is not None else set()
    out = set()
    for n in names:
        if n in seen or n not in protos:
            continue
        seen.add(n)
        out |= protos[n]["required"]
        out |= required_of(protos, protos[n]["inherits"], seen)
    return out


def source_methods(path):
    """class -> selectors implemented in our own .m, categories included."""
    out = {}
    cur = None
    pending = None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"^@implementation (\w+)", line)
            if m:
                cur = m.group(1)
                out.setdefault(cur, set())
                continue
            if line.startswith("@end"):
                cur, pending = None, None
                continue
            if cur is None:
                continue
            if pending is None and re.match(r"^[-+]\s*\(", line):
                pending = line
            elif pending is not None:
                pending += line
            if pending is not None and ("{" in pending or ";" in pending):
                # the selector is the keywords: `name:` pieces, or a bare name
                sig = pending.split("{")[0]
                sig = re.sub(r"^[-+]\s*\([^)]*\)", "", sig, count=1)
                sig = re.sub(r"\([^()]*\)", " ", sig)          # argument types
                parts = re.findall(r"(\w+)\s*:", sig)
                if parts:
                    out[cur].add("".join(p + ":" for p in parts))
                else:
                    bare = re.match(r"\s*(\w+)", sig)
                    if bare:
                        out[cur].add(bare.group(1))
                pending = None
    return out


def base_verdicts(images, dumps, selmap):
    """(class, selector) -> "abstract" / "nil-stub" / "real", read out of the images.

    The name and the body come from two different places and are joined by
    POSITION: ipsw resolves the cache's uniqued selector table, which an
    extracted image does not contain, and its listing is in method-list order.
    The join is then checked, not trusted -- a selector's colon count must equal
    the number of arguments in its type encoding -- and what does not check out
    is left out and counted."""
    for path in selmap:
        protometh.load_selmap(path)
    imgs, verdicts, mismatch = {}, {}, 0
    for path in images:
        imgs[os.path.basename(path)] = protometh.Image(path)
    for img in imgs.values():
        img.link(list(imgs.values()))
    names = {}
    for path in dumps:
        for cls, sels in protometh.load_names(path).items():
            names.setdefault(cls, sels)
    for img in imgs.values():
        for cls, c in protometh.classes(img).items():
            listed = names.get(cls)
            if listed is None or len(listed) != len(c["methods"]):
                continue
            for (sel, types, imp), name in zip(c["methods"], listed):
                if protometh.argument_count(types) != name.count(":"):
                    mismatch += 1
                    continue
                verdicts[(cls, name)] = (protometh.classify_imp(img, imp), types)
    return verdicts, mismatch


def our_encodings(path):
    """class -> {selector: encoding} out of the assembly clang generates for us.

    The object file cannot be read instead: a selector reference in it is a
    relocation, zero on disk. The assembly still names the implementation
    symbol, and the implementation symbol spells the selector."""
    text = open(path, errors="replace").read()
    types = dict(re.findall(
        r'^(l_OBJC_METH_VAR_TYPE_[\w.]*):[^\n]*\n\s*\.asciz\s+"([^"]*)"', text, re.M))
    out = {}
    for block in re.finditer(
            r'^__OBJC_\$_INSTANCE_METHODS_(\w+):\n((?:\s+\.(?:long|quad)\t.*\n)+)',
            text, re.M):
        cls, body = block.group(1), block.group(2)
        for _, tlabel, sel in re.findall(
                r'\.quad\t(l_OBJC_METH_VAR_NAME_[\w.]*)\n\s*\.quad\t'
                r'(l_OBJC_METH_VAR_TYPE_[\w.]*)\n\s*\.quad\t"-\[\w+ ([^\]]+)\]"', body):
            out.setdefault(cls, {})[sel] = types.get(tlabel, "?")
    return out


def declared_encodings(images, selmap):
    """selector -> {encoding} as the protocols in these images declare it."""
    for path in selmap:
        protometh.load_selmap(path)
    imgs = [protometh.Image(p) for p in images]
    for img in imgs:
        img.link(imgs)
    want = {}
    for img in imgs:
        for _, p in protometh.protocols(img).items():
            for kind in ("required", "optional"):
                for sel, types, _ in p[kind]:
                    if sel and types:
                        want.setdefault(sel, set()).add(types)
    return want


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="append", default=[], required=True)
    ap.add_argument("--src", default=os.path.join(ROOT, "userland/src/redwoodmetal.m"))
    ap.add_argument("--class", dest="only", action="append", default=[])
    ap.add_argument("--images", help="directory of images extracted with `ipsw dyld extract`")
    ap.add_argument("--selmap", action="append", default=[],
                    help="output of `ipsw dyld objc sel <DSC> -i <image>`")
    ap.add_argument("--asm", help="assembly of redwoodmetal.m, for the signature check")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    protos, clss = read_dumps(args.dump)
    mine = source_methods(args.src)
    names = args.only or list(CLASSES)
    images = []
    if args.images:
        images = [os.path.join(args.images, f) for f in sorted(os.listdir(args.images))]
    verdicts, joined_badly = base_verdicts(images, args.dump, args.selmap) if images \
        else ({}, 0)
    ours_enc = our_encodings(args.asm) if args.asm else {}
    want_enc = declared_encodings(images, args.selmap) if images and args.asm else {}
    total_required = total_unanswered = total_abstract = 0

    for name in names:
        spec = CLASSES.get(name)
        if spec is None:
            print("owes: %s is not in the table" % name, file=sys.stderr)
            continue
        # NSObject's 19 are left out of the count, as the test on the phone leaves them
        # out: the root class answers them, and a number that includes them cannot be
        # compared with the one the runtime prints (protocol_copyMethodDescriptionList
        # does not walk the protocols a protocol adopts).
        req = required_of(protos, spec["protocols"]) - required_of(protos, ["NSObject"])
        ours = mine.get(name, set())
        base = set()
        abstract, silent, unread = [], [], []
        for b in spec["bases"]:
            answered = clss.get(b, {"methods": set()})["methods"]
            base |= answered
            if verdicts and answered and not any((b, sel) in verdicts for sel in answered):
                # the names and the bodies of this base class could not be joined (its
                # listing and its method list are of different lengths, which a category
                # in the same image does), so nothing of it was classified and the two
                # columns below say nothing about it
                unread.append(b)
            for sel in sorted(answered & (req - ours)):
                verdict = verdicts.get((b, sel), ("", ""))[0]
                if verdict == "abstract":
                    abstract.append("%s (%s)" % (sel, b))
                elif verdict == "nil-stub":
                    # not an abort: Apple's other way of saying "override me", which
                    # answers zero, nil or nothing and says nothing at all
                    silent.append("%s (%s)" % (sel, b))
        # NSObject's own methods are not in these images; the 19 selectors the
        # NSObject protocol requires are all answered by the root class, and
        # treating them as answered is the one assumption here.
        base |= required_of(protos, ["NSObject"])
        unanswered = sorted(req - ours - base)
        total_required += len(req)
        total_unanswered += len(unanswered)
        total_abstract += len(abstract)
        print("%-30s %4d required, %3d by us, %3d by the base, %3d UNANSWERED, "
              "%2d ABSTRACT IN THE BASE, %3d silent in the base"
              % (name, len(req), len(req & ours), len(req & base - ours),
                 len(unanswered), len(abstract), len(silent)))
        if unread:
            print("%-30s   the bodies of %s were not classified, so its entries are "
                  "counted as answered and nothing is said about what they do"
                  % ("", ", ".join(unread)))
        if args.verbose:
            for sel in unanswered:
                print("    unanswered  %s" % sel)
            for sel in abstract:
                print("    abstract    %s" % sel)
    print("%-30s %4d required, %34d UNANSWERED, %2d ABSTRACT IN THE BASE"
          % ("total", total_required, total_unanswered, total_abstract))
    if images:
        print("owes: %d (class, selector) pairs of Apple's were classified, %d left out "
              "because the name and the encoding disagreed about the argument count"
              % (len(verdicts), joined_badly))

    if ours_enc and want_enc:
        checked = bad = 0
        for name in names:
            spec = CLASSES.get(name)
            if spec is None:
                continue
            req = required_of(protos, spec["protocols"])
            for sel, types in sorted(ours_enc.get(name, {}).items()):
                if sel not in req:
                    continue
                checked += 1
                expect = want_enc.get(sel)
                if expect and types not in expect:
                    bad += 1
                    print("owes: SIGNATURE %-26s %-50s ours %-22s Apple %s"
                          % (name, sel, types, " | ".join(sorted(expect))))
        print("owes: %d of our implementations were checked against the encodings the "
              "protocols declare, %d disagree" % (checked, bad))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
