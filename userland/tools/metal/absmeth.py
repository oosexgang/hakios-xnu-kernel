#!/usr/bin/env python3
"""Which methods of a class in the shared cache actually do something.

Apple writes an abstract method as a real entry in the method list whose body is
`[self doesNotRecognizeSelector:_cmd]`. That is indistinguishable from a working
method through the Objective-C runtime -- `class_copyMethodList` lists it,
`instancesRespondToSelector:` says yes -- and when it refuses, the exception
names the RECEIVER's class, so it reads as if the method were missing from a
subclass of ours. The first device probe of subsystem (2) read
"initWithAcceleratorPort: yes, its own" out of the runtime and then died on
"-[RedwoodMetalDevice initWithAcceleratorPort:]: unrecognized selector". Both
lines were true.

So the question "what does a driver of ours owe?" cannot be answered from the
runtime at all. It is answered here, from the cache: resolve each selector to its
implementation, disassemble the first instructions, and sort them into

  abstract   its body calls doesNotRecognizeSelector:, so a subclass must
             implement it -- this is the work order
  ivar       a load and a return: a property accessor, which a subclass gets for
             free once the ivar is filled
  real       anything else, printed with the first thing it calls, because that
             names where the behaviour comes from

usage: absmeth.py --class _MTLDevice [--image PATH] [--sel S ...] [--all]

With neither --sel nor --all it classifies DRIVER_SELECTORS, the selectors
docs/metal.md section 1 found Metal sending to a device object.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.environ.get(
    "DSC", os.path.join(ROOT, "ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64"))
METAL = "/System/Library/Frameworks/Metal.framework/Metal"
# addresses and names are stable for one cache, and ipsw pays for the whole cache
# on every invocation, so remember what it said
MEMO = os.environ.get("ABSMETH_CACHE",
                      os.path.join(tempfile.gettempdir(), "absmeth-memo.json"))

# The selectors a device object is known or suspected to owe, from the discovery
# chain in docs/metal.md section 1 and from MTLDevice's public protocol.
DRIVER_SELECTORS = [
    "initWithAcceleratorPort:",
    "name",
    "registryID",
    "maxThreadsPerThreadgroup",
    "recommendedMaxWorkingSetSize",
    "hasUnifiedMemory",
    "supportsFamily:",
    "supportsFeatureSet:",
    "newCommandQueue",
    "newCommandQueueWithMaxCommandBufferCount:",
    "newBufferWithLength:options:",
    "newBufferWithBytes:length:options:",
    "newTextureWithDescriptor:",
    "newLibraryWithData:error:",
    "newLibraryWithSource:options:error:",
    "newDefaultLibrary",
    "newComputePipelineStateWithFunction:error:",
    "newRenderPipelineStateWithDescriptor:error:",
    "newSamplerStateWithDescriptor:",
    "newDepthStencilStateWithDescriptor:",
    "newHeapWithDescriptor:",
    "newFence",
    "newEvent",
]

memo = {}


def load_memo():
    global memo
    try:
        with open(MEMO) as f:
            memo = json.load(f)
    except (OSError, ValueError):
        memo = {}


def save_memo():
    try:
        with open(MEMO, "w") as f:
            json.dump(memo, f)
    except OSError:
        pass


def ipsw(args, key):
    if key in memo:
        return memo[key]
    out = subprocess.run(["ipsw", "dyld"] + args, capture_output=True,
                         timeout=900).stdout.decode(errors="replace")
    memo[key] = out
    return out


def address_of(image, cls, sel):
    name = "-[%s %s]" % (cls, sel)
    out = ipsw(["symaddr", CACHE, "-i", image, "--", name], "sym:" + image + name)
    m = re.search(r"^(0x[0-9a-f]+):", out, re.M)
    return int(m.group(1), 16) if m else None


def symbol_at(addr):
    out = ipsw(["a2s", CACHE, hex(addr)], "a2s:%x" % addr)
    m = re.search(r"^0x[0-9a-f]+:\s*(.+?)\s*$", out, re.M)
    return m.group(1) if m else hex(addr)


def body(addr, count=28):
    """[(address, mnemonic, operands)] from the implementation's first instructions."""
    out = ipsw(["disass", CACHE, "-q", "-a", hex(addr), "-c", str(count)],
               "dis:%x:%d" % (addr, count))
    insns = []
    for line in out.splitlines():
        m = re.match(r"^(0x[0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return insns


def classify(addr):
    """(verdict, instructions to the end of the body, what it calls or forwards to).

    Finding the end of the body matters more than it looks. The first version of
    this stopped only at `ret` while reading a fixed window, so for a method two
    instructions long it went on reading its NEIGHBOURS -- and since Metal packs
    runs of two-instruction forwarders together, it found a tail call to
    `doesNotRecognizeSelector:` two methods later and called the first one
    abstract. `-[_MTLDevice supportsMeshShaders]` (0x181aa8bb4) was reported that
    way and is really `ldr x0, [x0, #0x160]; b <stub>`: it forwards to the
    feature-query object. So a tail call ends the body as surely as a return, and
    the verdicts separate forwarding from answering nothing:

      abstract   calls doesNotRecognizeSelector: -- a subclass MUST implement it
      nil-stub   at most three instructions returning zero, nil or nothing; this
                 is Apple's other way of saying "a subclass overrides this", and
                 it says it silently
      const      the same shape, but what it returns is a LITERAL: a constant
                 string or a nonzero immediate, printed as its address. This is
                 an ANSWER, not an abdication, and it was worth separating from
                 nil-stub the day a nil-stub cost a crash (docs/metal.md 10.40):
                 -[_MTLDevice vendorName] is `adrp; add; ret` over @"Apple" and
                 was being counted among the methods that answer nothing
      ivar       reads an ivar and returns it
      forward    tail-calls something else, which is named
      real       anything longer

    The count is suffixed with "+" when the window ended before the body did."""
    insns = body(addr)
    if not insns:
        return "unreadable", "0", ""
    last = insns[-1][0]
    n, done = len(insns), False
    for i, (_, mn, ops) in enumerate(insns):
        if mn == "ret":
            n, done = i + 1, True
            break
        # an unconditional branch out of the range being read is a tail call, and
        # the body ends there; one that lands inside it is a jump
        if mn == "b" and ops.startswith("0x"):
            target = int(ops.split()[0], 16)
            if target < addr or target > last:
                n, done = i + 1, True
                break
    first_call, tail = "", ""
    for _, mn, ops in insns[:n]:
        if mn in ("bl", "b") and ops.startswith("0x"):
            target = int(ops.split()[0], 16)
            if target < addr or target > last:
                name = symbol_at(target)
                if "doesNotRecognizeSelector" in name:
                    return "abstract", "%d" % n if done else "%d+" % n, name
                if mn == "b":
                    tail = name
                elif not first_call:
                    first_call = name
    count = "%d" % n if done else "%d+" % n
    if done and n <= 3 and not first_call and not tail:
        if any(mn.startswith("ldr") for _, mn, _ in insns[:n]):
            return "ivar", count, ""
        page = None
        for _, mn, ops in insns[:n]:
            if mn == "adrp":
                m = re.search(r"(0x[0-9a-f]+)", ops)
                page = int(m.group(1), 16) if m else None
            elif mn == "add" and page is None and re.match(r"^x0, x0, #", ops):
                # `add x0, x0, #off; ret` hands back the ADDRESS of an embedded ivar,
                # which is how Apple returns a struct that lives inside the object:
                # -[_MTLDevice limits] is self+8, the 64-field limits table that
                # -initLimits fills, and -getBVHBuilderLock is self+0x1b0. Counting
                # those as nil-stubs said "answers nothing" about two methods that
                # answer correctly for free, and would have predicted a crash in
                # -maxFragmentTextures, which reads limits+0x90 unchecked and works.
                return "ivar", count, "self+" + ops.split("#")[-1].strip()
            elif mn == "add" and page is not None:
                m = re.search(r"#(0x[0-9a-f]+|\d+)", ops)
                off = int(m.group(1), 16) if m and m.group(1).startswith("0x") \
                    else int(m.group(1)) if m else 0
                return "const", count, hex(page + off)
            elif mn.startswith("mov") and re.search(r"#(?!0\b|0x0\b)", ops):
                return "const", count, ops.split("#")[-1]
        if page is not None:
            return "const", count, hex(page)
        return "nil-stub", count, ""
    if tail and n <= 4 and not first_call:
        return "forward", count, tail
    return "real", count, first_call or tail


def selectors_of(image, cls):
    """Every instance method the class declares, from ipsw's ObjC dump."""
    out = ipsw(["macho", CACHE, image, "--objc"], "objc:" + image)
    want = re.compile(r"^-\[%s (.+)\];$" % re.escape(cls), re.M)
    return want.findall(out)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--class", dest="cls", required=True)
    ap.add_argument("--image", default=METAL)
    ap.add_argument("--sel", action="append", default=[])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args(argv)

    if not os.path.exists(CACHE):
        print("absmeth: no shared cache at %s (set DSC)" % CACHE, file=sys.stderr)
        return 1
    load_memo()

    if args.all:
        sels = selectors_of(args.image, args.cls)
        if not sels:
            print("absmeth: %s declares no instance methods in %s"
                  % (args.cls, args.image), file=sys.stderr)
            return 1
    else:
        sels = args.sel or DRIVER_SELECTORS

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        addrs = list(pool.map(lambda s: address_of(args.image, args.cls, s), sels))
        def one(pair):
            sel, addr = pair
            if addr is None:
                return (sel, None, "no entry", "-", "")
            return (sel, addr) + classify(addr)

        rows = list(pool.map(one, zip(sels, addrs)))
    save_memo()

    order = {"abstract": 0, "nil-stub": 1, "no entry": 2, "const": 3, "ivar": 4,
             "forward": 5, "real": 6, "unreadable": 7}
    rows.sort(key=lambda r: (order.get(r[2], 9), r[0]))
    print("absmeth: %s in %s" % (args.cls, os.path.basename(args.image)))
    for sel, addr, verdict, n, call in rows:
        where = "%#x" % addr if addr is not None else "-"
        extra = ""
        if verdict == "forward":
            extra = "  tail-calls %s" % call
        elif verdict == "real" and call:
            extra = "  calls %s" % call
        elif verdict == "real":
            extra = "  calls nothing"
        elif call:
            extra = "  returns %s" % call
        print("  %-11s %-46s %-12s %4s insn%s" % (verdict, sel, where, n, extra))

    counts = {}
    for _, _, verdict, _, _ in rows:
        counts[verdict] = counts.get(verdict, 0) + 1
    print()
    print("absmeth: %d selectors: %s" % (
        len(rows), ", ".join("%d %s" % (v, k) for k, v in sorted(counts.items()))))
    print("absmeth: `abstract` is a method a subclass must write, and the Objective-C runtime "
          "cannot tell it from a working one. `nil-stub` answers zero, nil or nothing, and only "
          "the selector says whether that is an abdication (-compiler returning nil) or the "
          "answer itself (-hasUnifiedMemory returning NO), so read those one at a time -- and "
          "read them first, because a caller that asked respondsToSelector: and got YES does not "
          "ask again (docs/metal.md 10.40). `const` returns a literal, which is an answer. `ivar` "
          "needs the ivar filled rather than a method, `forward` works once what it forwards to "
          "exists, and `real` is Apple's code, which runs for us if what it calls is there.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
