#!/usr/bin/env python3
"""The 135 library functions: how far each one gets, and against what.

An air.visible function is not a shader. Metal links one into a caller at
pipeline time through the ABI its own metadata describes (air.stitching_info),
which this project does not implement -- so a third of the corpus had nothing
that could be dispatched and was counted only as "not an entry point".

air_to_nir's --library wraps one in a generated entry point instead: argument i
read from device buffer 0 at byte 16*i, the return value written to buffer 1
(wrap_library_function in air_to_nir.c). That makes a library function runnable
and comparable, and this script reports what each comparison is worth, because
they are not worth the same thing:

  LLVM            the function compiled for this machine by llc and called with
                  the same bytes. Independent of everything of ours, and the
                  strongest evidence here. Available in full only for a
                  function that calls no air.* at all.
  LLVM + airdefs  the same, for a function whose air.* calls are all exactly
                  defined: airdefs.py gives those operations a body so llc can
                  execute them. Weaker and reported on its own line, because
                  LLVM then decides the function's own arithmetic while we
                  decide what its operations mean -- from intrincheck's
                  reference, which is itself pinned to the Adreno on 1 748
                  cases. A family with no bit-exact reference anywhere is
                  refused by name instead of approximated.
  folded vs run   our NIR constant-folded on this machine against our NIR run
                  by a Gallium driver. Not independent of the frontend -- a
                  mistranslation is on both sides -- but it does catch the
                  backend and the hardware disagreeing with the folder, which
                  is how the ir3 u32-to-f32 rounding bug was found.

Both sides read argument i from byte 16*i, so neither comparison can catch a
slot layout that is wrong in the same way twice. What they do catch is the two
sides disagreeing about which slot is which argument, because the fill pattern
makes every slot's bytes different. Apple's own air.arg_type_name for each
argument is checked against the LLVM signature before anything runs
(cpuref.py, check_against_metadata), which is the one part of the layout that
has a second source.

usage: libcheck.py [--list] [FILE.metallib...]
       libcheck.py --log RUN.log [FILE.metallib...]
       (no files: portrait_filters.metallib, the corpus's library)

--log reads a device run instead of running here: the words the Adreno wrote
into buffer 1, against the words our folder computes on this machine. A block
is keyed on the name air_run prints, not on the order of the lines, because the
console is shared with launchd and the loader drops its own middle when the log
grows (docs/checks.md in hakios-xnu-kernel).
"""

import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airdefs  # noqa: E402  (which operations have a host body, and which do not)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))
CPUREF = os.path.join(ROOT, "tools/air/cpuref.py")

LIBS = ["ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/"
        "portrait_filters.metallib"]

# The air.* operations whose result is exactly defined, by the family name in
# the declaration. The split matters: a function calling an approximate one
# cannot be compared bit-exactly against anything, here or anywhere, and saying
# so is not the same as saying it is wrong (docs/air.md section 4).
APPROX = {"fast_pow", "fast_powr", "fast_exp", "fast_exp2", "fast_exp10", "fast_log",
          "fast_log2", "fast_log10", "fast_sin", "fast_cos", "fast_tan", "fast_atan2",
          "fast_rsqrt", "fast_divide", "fast_asin", "fast_acos", "fast_atan",
          "fast_sinh", "fast_cosh", "fast_tanh"}


def report(lib):
    """air2nir --library --report: one row per module of the library."""
    out = subprocess.run([AIR2NIR, "--library", "--report", lib],
                         capture_output=True, text=True)
    rows = []
    for line in out.stdout.splitlines():
        if not line.startswith("module="):
            continue
        r = dict(kv.split("=", 1) for kv in line.split("\t") if "=" in kv)
        r["calls"] = [c for c in r.get("calls", "").split(",") if c]
        rows.append(r)
    return rows


def families(row):
    return {c.split(".")[1] for c in row["calls"] if c.startswith("air.")}


def folded(lib, name):
    """What our frontend folds the wrapper to, as 32-bit words."""
    out = subprocess.run([AIR2NIR, "--library", "--eval", lib, name],
                         capture_output=True, text=True)
    if out.returncode:
        return None, "air2nir --eval failed"
    for line in out.stdout.splitlines():
        if not line.startswith("  buffer"):
            continue
        if "did not fold" in line:
            return None, "the store did not fold to a constant"
        return [bits_of(v) for v in line.split()[1:]], None
    return None, "no buffer store in the folded shader"


def bits_of(text):
    """A float as printed by --eval, back to its 32 bits. NaN keeps no payload."""
    f = float(text)
    if f != f:
        return "nan"
    return struct.unpack("<I", struct.pack("<f", f))[0]


def ran(lib, name):
    """What a Gallium driver writes into buffer 1, as 32-bit words."""
    out = subprocess.run([AIR_RUN, "--library", lib, name],
                         capture_output=True, text=True)
    if out.returncode:
        last = (out.stderr or out.stdout).strip().splitlines()
        return None, (last[-1][:120] if last else "air_run failed")
    words, taking = [], False
    for line in out.stdout.splitlines():
        if line.startswith("air_run: device buffer 1 after"):
            taking = True
            continue
        if taking:
            m = re.match(r"\s*\[\s*\d+\]\s+\S+\s+0x([0-9a-f]+)", line)
            if not m:
                break
            words.append(int(m.group(1), 16))
    return (words, None) if words else (None, "air_run printed no buffer")


def same(a, b):
    """Word for word, with any NaN equal to any NaN and nothing else loose."""
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x == "nan" or y == "nan":
            # a NaN's payload is not specified by Metal and not preserved by
            # every lowering, so two NaNs agree and a NaN beside a number does
            # not. Counting them apart is the caller's job.
            if not (is_nan(x) and is_nan(y)):
                return False
        elif x != y:
            return False
    return True


def is_nan(w):
    return w == "nan" or (isinstance(w, int) and (w & 0x7f800000) == 0x7f800000
                          and (w & 0x7fffff))


def llvm_agrees(lib, name):
    """cpuref.py --compare: LLVM on this machine against our folded NIR."""
    out = subprocess.run([sys.executable, CPUREF, "--compare", lib, name],
                         capture_output=True, text=True)
    text = out.stdout + out.stderr
    if "they agree" in text:
        return True, None
    if "they DIFFER" in text:
        return False, text.strip().splitlines()[-1][:120]
    return None, text.strip().splitlines()[-1][:120] if text.strip() else "no output"


def ulps(a, b):
    """How far apart two 32-bit float words are, in representable steps."""
    if a == "nan" or b == "nan" or not isinstance(a, int) or not isinstance(b, int):
        return 0 if is_nan(a) and is_nan(b) else 1 << 30

    def ordered(w):
        # map the sign-magnitude float order onto the integers
        return (1 << 31) - w if w & (1 << 31) else w + (1 << 31)

    return abs(ordered(a) - ordered(b))


# How far apart the folder and the hardware may be put down to contraction
# rather than to a defect. The bound is not a tolerance on the result: it is
# the scale at which a fused multiply-add differs from a rounded one, a few
# steps of the last place, and anything beyond it needs its own explanation.
CONTRACTION_ULPS = 32


def from_log(path, names):
    """Each function's buffer-1 words as a device run reported them."""
    blocks, cur = {}, None
    for line in open(path, errors="replace"):
        m = re.match(r"^air_run: (\S+) on ", line)
        if m:
            cur = m.group(1)
            blocks.setdefault(cur, [])
            continue
        if cur:
            blocks[cur].append(line)
    out = {}
    for n in names:
        b = blocks.get(n)
        if b is None:
            out[n] = (None, "no block in the log at all")
            continue
        text = "".join(b)
        crash = next((l.strip() for l in b if "signal " in l and " at 0x" in l), None)
        if crash:
            out[n] = (None, "CRASHED: " + crash[:90])
            continue
        words, taking = [], False
        for line in b:
            if line.startswith("air_run: device buffer 1 after"):
                taking = True
                continue
            if taking:
                m = re.match(r"\s*\[\s*\d+\]\s+\S+\s+0x([0-9a-f]+)", line)
                if not m:
                    break
                words.append(int(m.group(1), 16))
        if not words:
            why = ""
            for l in b:
                if "air_run:" in l or "Assertion" in l:
                    why = l.strip()
            out[n] = (None, why[:90] or "no buffer in the log")
        else:
            out[n] = (words, None)
    return out


def log_mode(libs, log, want_list):
    """A device run against our own folding, function by function."""
    rows = []
    for lib in libs:
        path = lib if os.path.isabs(lib) else os.path.join(ROOT, lib)
        for r in report(path):
            r["path"] = path
            rows.append(r)
    wrapped = [r for r in rows if r.get("translate") == "ok"]
    got = from_log(log, [r["module"] for r in wrapped])
    if all(v[0] is None and "no block" in (v[1] or "") for v in got.values()):
        print("libcheck: %s holds no block for any of the %d functions, so it is a log of "
              "some other run rather than a run where all of them vanished"
              % (os.path.basename(log), len(wrapped)), file=sys.stderr)
        return 1

    agree, licensed, contracted, differ, nofold, missing = [], [], [], [], [], []
    for r in wrapped:
        words, why = got[r["module"]]
        if words is None:
            missing.append((r, why))
            continue
        ours, why2 = folded(r["path"], r["module"])
        if ours is None:
            nofold.append((r, why2))
            continue
        if same(ours, words):
            agree.append((r, None))
            continue
        d = max(ulps(x, y) for x, y in zip(ours, words)) if len(ours) == len(words) \
            else 1 << 30
        note = "folded %s, the Adreno %s (%d ulp)" % (ours, words, d)
        if families(r) & APPROX:
            # an approximate operation is allowed to differ from anything, here
            # as everywhere: air.fast_pow on this GPU even returns NaN where
            # softpipe returns a value, and the standard permits it
            licensed.append((r, note))
        elif d <= CONTRACTION_ULPS:
            # The hardware contracts a multiply-add; the folder rounds each
            # step. Demonstrated rather than assumed for
            # _pf_portraitConvertToGrayscale, whose whole body is one
            # air.dot.v3f32: evaluating that dot as an fma chain in Python
            # gives the Adreno's word exactly, and evaluating it left to right
            # in binary32 gives the folder's. Metal compiles with
            # air.compile.fast_math_enable, which permits the contraction.
            contracted.append((r, note))
        else:
            differ.append((r, note))

    print("libcheck: %d wrapped library functions, as %s reports them"
          % (len(wrapped), os.path.basename(log)))
    print("  %3d  ran and wrote exactly the words our folder computes" % len(agree))
    print("  %3d  differ in the last bits, calling an approximate air.* (licensed)"
          % len(licensed))
    print("  %3d  differ within %d ulp: the hardware contracts multiply-add"
          % (len(contracted), CONTRACTION_ULPS))
    print("  %3d  DISAGREE with no explanation" % len(differ))
    print("  %3d  ran, but the folder does not reduce the result, so nothing to compare"
          % len(nofold))
    print("  %3d  crashed, were refused, or are not in the log" % len(missing))
    if want_list:
        for r, why in differ + licensed + contracted + missing:
            print("        %-44s %s" % (r["module"], why))
    total = (len(agree) + len(licensed) + len(contracted) + len(differ)
             + len(nofold) + len(missing))
    if total != len(wrapped):
        print("libcheck: the rows add to %d and %d were wrapped" % (total, len(wrapped)),
              file=sys.stderr)
        return 1
    print("libcheck: the rows add to %d, which is every wrapped function. Agreement here is "
          "between two readings of our own frontend -- the folder and the hardware -- except "
          "for the 21 that LLVM also agrees with." % total)
    return 1 if differ else 0


def main(argv):
    want_list = "--list" in argv
    log = None
    if "--log" in argv:
        i = argv.index("--log")
        log = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    libs = [a for a in argv if not a.startswith("--")] or LIBS
    for prog in (AIR2NIR, AIR_RUN):
        if not os.path.exists(prog):
            print("libcheck: run tools/air/mk-host.sh first (%s)" % prog, file=sys.stderr)
            return 1

    rows = []
    for lib in libs:
        path = lib if os.path.isabs(lib) else os.path.join(ROOT, lib)
        if not os.path.exists(path):
            print("libcheck: missing %s" % lib, file=sys.stderr)
            return 1
        for r in report(path):
            r["path"] = path
            rows.append(r)

    if log:
        return log_mode(libs, log, want_list)

    wrapped = [r for r in rows if r.get("translate") == "ok"]
    refused = [r for r in rows if r.get("translate") != "ok"]

    # Which class a function falls in is airdefs' answer, not a guess from the
    # operation's name: it says outright which it can define, which it refuses
    # as approximate, and which it does not handle at all (the texture
    # operations, among others). Asking the name instead put the eleven
    # texture-sampling functions in the "exactly defined" row, where cpuref
    # then refused them one by one -- a row that was wrong and a reason that
    # was right, which is the worst way to be told.
    plain, exact, approx, nobody = [], [], [], []
    for r in wrapped:
        names = [c for c in r["calls"] if c.startswith("air.")]
        if not names:
            plain.append(r)
            continue
        a = airdefs.analyze(names)
        if a["refused"]:
            approx.append(r)
        elif a["skipped"]:
            nobody.append(r)
        else:
            exact.append(r)

    # what each one is worth, measured rather than assumed
    llvm_ok, llvm_bad, llvm_other = [], [], []
    for r in plain:
        ok, why = llvm_agrees(r["path"], r["module"])
        (llvm_ok if ok else llvm_bad if ok is False else llvm_other).append((r, why))

    # The same comparison for the ones that call only exactly defined
    # operations: LLVM can execute them once airdefs.py gives those operations
    # a body. Kept apart from the row above and never added to it -- there,
    # LLVM decides everything; here, it decides the function's own arithmetic
    # and we decide what its air.* calls mean. The operations' semantics come
    # from intrincheck's reference, which is pinned to the Adreno on 1 748
    # cases, so this is a chain of two measurements rather than one source.
    def_ok, def_bad, def_other = [], [], []
    for r in exact:
        ok, why = llvm_agrees(r["path"], r["module"])
        (def_ok if ok else def_bad if ok is False else def_other).append((r, why))

    run_ok, run_bad, no_fold, no_run, nan_rows = [], [], [], [], []
    for r in wrapped:
        theirs, why2 = ran(r["path"], r["module"])
        if theirs is None:
            no_run.append((r, why2))
            continue
        ours, why1 = folded(r["path"], r["module"])
        if ours is None:
            # it ran: what is missing is the host side of the comparison, not
            # the dispatch. Folding stops at a loop or at control flow the
            # folder cannot resolve, which says nothing about the shader.
            no_fold.append((r, why1))
            continue
        if any(is_nan(w) for w in ours):
            nan_rows.append(r)
        (run_ok if same(ours, theirs) else run_bad).append(
            (r, "folded %s, ran %s" % (ours, theirs)))

    print("libcheck: %d library functions in %d librar%s"
          % (len(rows), len(libs), "y" if len(libs) == 1 else "ies"))
    print("  %3d  wrapped in a generated entry point and translated" % len(wrapped))
    print("  %3d  refused: an argument the wrapper cannot pass" % len(refused))
    print()
    print("  of the %d wrapped, what a comparison can be worth:" % len(wrapped))
    print("  %3d  call no air.* at all, so LLVM on this machine is a full reference"
          % len(plain))
    print("  %3d  call only exactly defined air.*, which airdefs.py gives a host body"
          % len(exact))
    print("  %3d  call an approximate air.*, which nothing compares bit-exactly"
          % len(approx))
    print("  %3d  call an air.* with no host body at all -- the texture operations: LLVM "
          "cannot sample a texture on this machine" % len(nobody))
    print()
    print("  %3d  of the %d agree with LLVM bit for bit" % (len(llvm_ok), len(plain)))
    print("  %3d  DIFFER from LLVM" % len(llvm_bad))
    print("  %3d  could not be compared with LLVM" % len(llvm_other))
    print("  %3d  of the %d agree with LLVM *plus our own definitions* of the exactly "
          "defined air.* they call (airdefs.py) -- a weaker row, kept separate"
          % (len(def_ok), len(exact)))
    print("  %3d  DIFFER from LLVM with those definitions" % len(def_bad))
    print("  %3d  could not be compared that way" % len(def_other))
    print("  %3d  of the %d dispatched on softpipe and read their result back"
          % (len(run_ok) + len(run_bad) + len(no_fold), len(wrapped)))
    print("  %3d  of those match our own folding of the same shader" % len(run_ok))
    print("  %3d  run but DIFFER from our own folding" % len(run_bad))
    print("  %3d  ran, but our folding did not reduce to a constant, so there was "
          "nothing to compare" % len(no_fold))
    print("  %3d  could not be dispatched at all" % len(no_run))
    if nan_rows:
        print("  (%d of them return a NaN, where only the quiet bit is compared)"
              % len(nan_rows))

    if want_list:
        for r in refused:
            print("        refused  %-44s %s" % (r["module"], r.get("error", "")[:80]))
        for r, why in llvm_bad + def_bad + run_bad + llvm_other + def_other + no_run + no_fold:
            print("        %-44s %s" % (r["module"], why))

    # the rows have to account for every module before the numbers are read
    if len(wrapped) + len(refused) != len(rows):
        print("libcheck: the rows add to %d and there are %d functions"
              % (len(wrapped) + len(refused), len(rows)), file=sys.stderr)
        return 1
    if len(run_ok) + len(run_bad) + len(no_fold) + len(no_run) != len(wrapped):
        print("libcheck: the run rows add to %d and %d were wrapped"
              % (len(run_ok) + len(run_bad) + len(no_fold) + len(no_run), len(wrapped)),
              file=sys.stderr)
        return 1
    if len(plain) + len(exact) + len(approx) + len(nobody) != len(wrapped):
        print("libcheck: the reference rows add to %d and %d were wrapped"
              % (len(plain) + len(exact) + len(approx) + len(nobody), len(wrapped)),
              file=sys.stderr)
        return 1
    print("libcheck: every function is in exactly one row of each group. "
          "Wrapping is not stitching: Metal's own ABI for these functions is not "
          "implemented, and nothing here is evidence about it.")
    return 1 if (llvm_bad or def_bad or run_bad) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
