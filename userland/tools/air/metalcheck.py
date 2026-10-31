#!/usr/bin/env python3
"""The Metal kernels the driver ran on the phone, against softpipe on the same input.

The driver compiles Apple's shader bitcode on the device and dispatches it on the
Adreno (docs/metal.md section 10.16). The first results were checked by hand --
`air_run --zero <lib> <kernel>` on this machine, the first four words read off
the screen, the same four words read out of the boot log, compared by eye -- and
that neither scales nor survives the person who did it. This is the same
comparison as a tool, so the claim is re-checkable and a regression shows up
instead of being believed.

The two sides:

  the device   a boot log, logs/<HHMMSS>-run.log. One process per kernel
               (userland/rc.jobs.metalcompute), printing through a console shared
               with launchd, so nothing may be inferred from the order of the
               lines; the loader drops the middle of a long log and its heartbeat
               reprints the console's tail, so a whole stretch can appear twice.
               Everything here is therefore keyed on the kernel's name, and a
               name that shows up twice with two different results is named as
               such rather than resolved by taking the first.
  the host     air_run --zero on softpipe, on the metallib of ios165/rootfs that
               the log names (the on-device path is the same string without the
               ios165/rootfs prefix).

Both sides must be the same computation before the numbers mean anything:

  the same input    zeros. air_run's default fill is (i * 7 + 13) & 0xff and the
                    device's test memsets its buffers to zero, so --zero is not a
                    convenience: without it the two sides read different bytes.
                    It also makes the two sides' notion of "written" the same
                    one -- with a zero fill, "this byte is non-zero" (what the
                    device counts) and "this buffer is not bit-identical to the
                    fill" (what air_run reports) are the same statement.
  the same grid     4x4x1 threads in one threadgroup, hard-coded on both sides.
                    For any kernel that writes per thread a different thread
                    count is a different computation, so the grid air_run prints
                    is checked rather than assumed.

What a comparison cannot be made is reported, never dropped and never counted as
a pass. The three that occur are kept apart because they mean different things:

  softpipe refuses the shader   an assertion out of nir_to_tgsi -- "Unknown NIR
                                opcode", `src_64`, its UBO component limit. That
                                is softpipe's limit and not ours: for those
                                kernels the device is the only reference there
                                is, and the kernel having finished on it is still
                                worth a row.
  nothing written on either     agreement, and a weak one: a buffer that is all
  side                          zeros on both sides is what a dispatch that did
                                nothing at all also produces. Never added to the
                                strong rows.
  the driver refused the        status 5 and a sentence. No kernel ran, so this
  dispatch                      is not a result about the kernel at all.

What agreement here does NOT prove: both sides compile Apple's bitcode with the
same frontend of ours (libredwoodair.dylib on the device is air_run's own path
through air_to_nir, tools/mk-mesa.sh), so a mistranslation of the bitcode is on
both sides and invisible here. What this catches is ir3 and the Adreno
disagreeing with nir_to_tgsi and softpipe. Independence from our frontend lives
in cpuref.py, where LLVM runs the module.

And zeros are not the kernels' real inputs: what belongs in those buffers is
CoreImage's knowledge, not the compiler's (kstruct.py). For a kernel that
initialises or reduces, zeros are an honest input; for the rest this measures
that the path works, not that the kernel is right.

usage: metalcheck.py --log RUN.log [--list]
       metalcheck.py --newest [--list]     the newest log that holds such a run
                                           ("no device log" and exit 0 if none)
"""

import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IOS = os.environ.get("IOS", os.path.join(ROOT, "ios165"))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))
# The job lists this could be a run of. There are several because the loader keeps
# only the first 256 KB and the last 1 MB of a boot's log: one list of 33 kernels
# lost the middle of its own table, so the list was split into batches. Which list
# a log is a run of is decided by overlap with the kernels found in it, and the
# choice is printed -- comparing a log against the wrong list would report most of
# another batch as lost.
JOB_GLOB = os.path.join(ROOT, "userland/rc.jobs.metal*")
LOGS = os.environ.get("LOGS", os.path.join(ROOT, "logs"))

# The grid and the fill both sides have to be using. Written out rather than
# derived, so that one side quietly changing is caught instead of followed.
GRID = "4x4x1 threads in one workgroup"
FILL = "ZEROS"
TIMEOUT = 180               # a kernel softpipe cannot finish is a named row, not a hang

# --- which numbering a log's statuses are in -------------------------------------------
#
# The driver's MTLCommandBufferStatus used to be its own invention -- committed 3,
# scheduled 4, completed 6, and 6 is not a status at all -- and since 2026-10-21 it is
# Metal's: NotEnqueued 0, Enqueued 1, Committed 2, Scheduled 3, Completed 4, Error 5
# (userland/src/redwoodmetal.m). The logs under logs/ are evidence and are not going to
# be regenerated, so this tool does not reinterpret their numbers and does not quietly
# follow the new ones: it DATES each log and reads it in the numbering it was written in.
#
# The driver says which that is, once per process, printed out of the very constants that
# answer -status:
#
#   redwoodmetal: MTLCommandBufferStatus: committed 2, scheduled 3, completed 4, error 5 -- ...
#
# A log without that line is older than the change, and the fallback is not a guess
# either: 6 cannot occur under Metal's numbering at all, so a log holding "status 6" is
# an old one whatever else it holds. Over all 141 logs in logs/ the only statuses the
# device's test ever printed are 5 (128 times) and 6 (990 times) -- never 4 -- so no old
# log is in doubt. The third case, no line and no 6, is said out loud instead of being
# assumed away: the loader keeps only the first 256 KB of a boot's log and the last 1 MB,
# so a NEW log can lose its banner in the cut, and 4 can only mean the new numbering.
OLD_SCHEME = (3, 4, 6, 5)   # committed, scheduled, completed, error -- before 2026-10-21
NEW_SCHEME = (2, 3, 4, 5)   # MTLCommandBufferStatus itself
RE_CB_SCHEME = re.compile(r"redwoodmetal: MTLCommandBufferStatus: committed (\d+), "
                          r"scheduled (\d+), completed (\d+), error (\d+)")
# every status the device's test printed, whichever of its lines it was on
RE_ANY_STATUS = re.compile(r"drvtest: [^\n]*?status (\d+)")
# What the test does with a status, which is NOT this tool's to change
# (userland/src/metal_drv_test.m compares [cmd status] with the literal 6 in five places
# and prints the literal "status 6" in a sixth): with the driver on Metal's numbers and
# the test still on the old ones, a buffer that completed is called a failure, so the row
# carries its status and no readback at all. That shape is named where it shows up rather
# than counted as a loss, because the fix for it is in that file.
TEST_KEYS_ON_SIX = ("the device's test still compares [cmd status] with the literal 6 "
                    "(userland/src/metal_drv_test.m): with the driver on Metal's numbers a "
                    "completed buffer is called a failure there, so the row carries a status "
                    "and no readback")

# What the device's test prints (userland/src/metal_drv_test.m). Matched by
# search and not anchored at the start of the line: the console is shared, and a
# line can carry another writer's bytes in front of this one.
RE_LIB = re.compile(r"drvtest: (/\S+\.metallib), (\d+) bytes")
RE_COMPILED = re.compile(r"drvtest: compute (\S+) +compiled: (\d+) threads per group, "
                         r"width (\d+), (\d+) bytes of threadgroup memory")
RE_CREFUSED = re.compile(r"drvtest: compute (\S+) +refused: (.*)")
RE_DISPATCH = re.compile(r"drvtest: dispatched (\S+): status (\d+)(?:, error: (.*))?")
RE_BUFFER = re.compile(r"drvtest: buffer (\d+): (\d+) non-zero bytes of (\d+), first words "
                       r"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})")
RE_NOTHING = re.compile(r"drvtest: nothing was written to either buffer")

# What air_run prints.
RE_H_GRID = re.compile(r"air_run: dispatching (.+?); (\d+) device buffers? bound, "
                       r"(\d+) of them written, filled with (\S+)")
RE_H_BUF = re.compile(r"air_run: device buffer (\d+) after the dispatch")
RE_H_SAME = re.compile(r"air_run: device buffer (\d+) came back bit-identical to the fill")
RE_H_WORD = re.compile(r"\s*\[\s*(\d+)\]\s+\S+\s+0x([0-9a-f]+)")
# Softpipe refusing a shader, by whichever of its own assertions fires. nir_to_tgsi's
# were the ones first met ("Unknown NIR opcode", src_64, the UBO component limit), but
# tgsi_ureg's fire too: AV1SW's five _Uint kernels abort air_run on
# `tgsi_ureg.h:893: ureg_swizzle: Assertion reg.File != TGSI_FILE_NULL`. Both are the
# software path's limit and neither says anything about the kernel or about the front
# end, so they belong in the same row -- and that row is NOT a failure. Keeping the
# tgsi_ureg one out of it made check.sh exit 1 for a reason that had nothing to do with
# what check.sh is about.
RE_H_NTT = re.compile(r"([^\n]*(?:nir_to_tgsi\.c|tgsi_ureg\.h|tgsi_ureg\.c|nir_to_tgsi\.h)"
                      r":\d+:[^\n]*Assertion[^\n]*)")


def nonzero_bytes(words):
    """How many of these words' bytes are non-zero -- the device's own measure."""
    return sum(1 for w in words for b in w.to_bytes(4, "little") if b)


def buflist(indices):
    return " ".join(str(i) for i in sorted(indices)) or "none"


_text = {}


def log_text(path):
    """The log as text. Bytes in, not text: these logs carry NUL bytes (hence `grep -a`),
    and the loader's own output is not always valid UTF-8."""
    if path not in _text:
        raw = open(path, "rb").read()
        _text[path] = raw.decode("utf-8", errors="replace").replace("\x00", "")
    return _text[path]


_schemes = {}


def status_scheme(path):
    """(completed, error, what to print about it first, and the one line that says it last).

    Both, because check.sh reads `tail -28` of this and nothing else: a dating printed
    only at the top is a dating that the one reader of these numbers never sees."""
    if path in _schemes:
        return _schemes[path]
    text = log_text(path)
    said = RE_CB_SCHEME.search(text)
    seen = sorted({int(s) for s in RE_ANY_STATUS.findall(text)})
    if said:
        numbers = tuple(int(g) for g in said.groups())
        note = ("metalcheck: this log names its own status numbering, out of the constants "
                "that answer -status: committed %d, scheduled %d, completed %d, error %d"
                % numbers)
        if numbers != NEW_SCHEME:
            note += ("\n  which is NOT MTLCommandBufferStatus (committed %d, scheduled %d, "
                     "completed %d, error %d); the log is read as it says, not as Metal "
                     "defines" % NEW_SCHEME)
        impossible = [s for s in seen if s > max(numbers)]
        if impossible:
            note += ("\n  and the test printed status %s, which that numbering does not "
                     "define: %s" % (", ".join(str(s) for s in impossible), TEST_KEYS_ON_SIX))
        scheme = (numbers[2], numbers[3], note,
                  "metalcheck: completion is %d in this log and the log says so itself "
                  "(MTLCommandBufferStatus; before 2026-10-21 this driver said 6)"
                  % numbers[2])
    elif OLD_SCHEME[2] in seen:
        scheme = (OLD_SCHEME[2], OLD_SCHEME[3],
                  "metalcheck: THIS LOG IS OLDER than the renumbering of 2026-10-21: it carries "
                  "no `redwoodmetal: MTLCommandBufferStatus:` line and it holds status %d, which "
                  "Metal does not define. Its numbers are the old ones -- committed %d, "
                  "scheduled %d, completed %d, error %d -- and the rows below are read that way, "
                  "so they say the same as they said before the change. A log from after it says "
                  "completed %d." % ((OLD_SCHEME[2],) + OLD_SCHEME + (NEW_SCHEME[2],)),
                  "metalcheck: the numbers above are this log's own, from BEFORE the "
                  "renumbering of 2026-10-21: completion is %d here, and %d in any log "
                  "written after it" % (OLD_SCHEME[2], NEW_SCHEME[2]))
    else:
        if NEW_SCHEME[2] in seen:
            why = ("it holds status %d, which only Metal's numbering has -- in the old one that "
                   "was 'scheduled', and no log written in it ever printed that number (all 141 "
                   "in logs/ hold only 5 and 6)" % NEW_SCHEME[2])
        elif seen:
            why = ("every status in it (%s) means the same under both numberings"
                   % ", ".join(str(s) for s in seen))
        else:
            why = "it holds no status line at all, so nothing in it depends on the answer"
        scheme = (NEW_SCHEME[2], NEW_SCHEME[3],
                  "metalcheck: this log carries no `redwoodmetal: MTLCommandBufferStatus:` line "
                  "and no status %d, so its numbering is not stated anywhere in it. Read as "
                  "Metal's (completed %d, error %d): %s, and that line can be lost in the "
                  "loader's cut."
                  % (OLD_SCHEME[2], NEW_SCHEME[2], NEW_SCHEME[3], why),
                  "metalcheck: this log does not say which status numbering it is in; the "
                  "numbers above are read as Metal's (completion %d)" % NEW_SCHEME[2])
    _schemes[path] = scheme
    return scheme


def read_log(path):
    """{kernel name: what the log says about it}, plus what could not be placed.

    Keyed on the name because the order of the lines cannot be trusted. The
    `buffer N:` and `nothing was written` lines do not name their kernel, so they
    are attached to the `dispatched NAME:` line above them, which is how the test
    prints them; a buffer line with no dispatch above it is a fragment of the
    loader's heartbeat echo and is counted rather than swallowed.
    """
    text = log_text(path)

    ks = {}        # name -> {libs, compiled, crefused, runs}

    def slot(name):
        return ks.setdefault(name, {"libs": set(), "compiled": set(),
                                    "crefused": set(), "runs": []})

    lib = None
    run = None      # the dispatch the buffer lines below belong to
    orphans = 0
    dropped = 0
    for line in text.splitlines():
        if "the log between the first" in line and "was dropped" in line:
            dropped += 1
            lib, run = None, None       # nothing survives across the cut
            continue
        m = RE_LIB.search(line)
        if m:
            lib = m.group(1)
            continue
        m = RE_COMPILED.search(line)
        if m:
            k = slot(m.group(1))
            k["libs"].add(lib)
            k["compiled"].add("%s/%s/%s" % (m.group(2), m.group(3), m.group(4)))
            continue
        m = RE_CREFUSED.search(line)
        if m:
            k = slot(m.group(1))
            k["libs"].add(lib)
            k["crefused"].add(m.group(2).strip())
            continue
        m = RE_DISPATCH.search(line)
        if m:
            k = slot(m.group(1))
            k["libs"].add(lib)
            run = {"status": int(m.group(2)), "error": (m.group(3) or "").strip(),
                   "bufs": {}, "nothing": False}
            k["runs"].append(run)
            continue
        m = RE_BUFFER.search(line)
        if m:
            if run is None:
                orphans += 1
                continue
            which, nz = int(m.group(1)), int(m.group(2))
            words = [int(m.group(4 + i), 16) for i in range(4)]
            # An older build of the test printed a line for every buffer,
            # including the untouched ones; zero non-zero bytes is not a write.
            if nz:
                run["bufs"][which] = {"nonzero": nz, "size": int(m.group(3)),
                                      "words": words}
            continue
        if RE_NOTHING.search(line):
            if run is not None:
                run["nothing"] = True
            continue

    # One run printed twice by the heartbeat is one run; two runs that disagree
    # are not something to pick between. Canonicalise and keep the distinct set.
    for name, k in ks.items():
        seen = {}
        for r in k["runs"]:
            key = (r["status"], r["error"], r["nothing"],
                   tuple(sorted((i, b["nonzero"], tuple(b["words"]))
                                for i, b in r["bufs"].items())))
            seen[key] = r
        k["distinct"] = list(seen.values())
    return ks, orphans, dropped


def host_run(lib, name):
    """air_run --zero on softpipe: ({buffer: words}, written set) or why not.

    Returns (result, row, note). `row` is None when there is a reference; it
    names the reason when there is not, and the reasons are deliberately not
    merged: nir_to_tgsi refusing a shader is softpipe's limit and says nothing
    about the kernel, while anything else is a tool that did not work.
    """
    try:
        out = subprocess.run([AIR_RUN, "--zero", lib, name],
                             capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, "hostfail", "air_run did not finish in %d s" % TIMEOUT
    text = out.stdout + out.stderr
    m = RE_H_NTT.search(text)
    if m:
        why = m.group(1).strip()
        op = re.search(r"Unknown NIR opcode: (\S+)", text)
        return None, "ntt", why + ("  [%s]" % op.group(1) if op else "")
    if out.returncode != 0:
        lines = [l.strip() for l in text.splitlines() if l.strip()]
        last = lines[-1][:110] if lines else "air_run failed with no output at all"
        # An assertion out of the software rasteriser is its limit, not a broken tool;
        # anything else really is a tool that did not work, and those two must not be
        # counted together.
        if "Assertion" in last and ("tgsi" in last or "softpipe" in last or "sp_" in last):
            return None, "ntt", last
        return None, "hostfail", last
    g = RE_H_GRID.search(text)
    if not g:
        return None, "hostfail", "air_run printed no dispatch line"
    # The comparison is only a comparison if the two sides ran the same thing.
    if g.group(1) != GRID:
        return None, "harness", "air_run dispatched %s, the device 4x4x1" % g.group(1)
    if not g.group(4).startswith(FILL):
        return None, "harness", "air_run filled the buffers with %s, not zeros" % g.group(4)

    bufs, cur = {}, None
    for line in text.splitlines():
        m = RE_H_BUF.search(line)
        if m:
            cur = int(m.group(1))
            bufs.setdefault(cur, {"words": [], "written": True})
            continue
        m = RE_H_SAME.search(line)
        if m:
            bufs.setdefault(int(m.group(1)), {"words": [], "written": True})
            bufs[int(m.group(1))]["written"] = False
            cur = None
            continue
        if cur is not None:
            w = RE_H_WORD.match(line)
            if w:
                bufs[cur]["words"].append(int(w.group(2), 16))
            else:
                cur = None
    if not bufs:
        return None, "hostfail", "air_run dispatched but printed no buffer"
    # air_run says up front how many buffers it is going to print; if the parse
    # above found a different number, the parse is wrong and not the shader.
    if len(bufs) != int(g.group(3)):
        return None, "hostfail", ("air_run says it wrote %s of %s buffers and printed %d"
                                  % (g.group(3), g.group(2), len(bufs)))
    return bufs, None, None


def compare(dev, host):
    """One dispatch against one host run: (row, note).

    `dev` is the device's distinct run, `host` what air_run printed. The device
    gives four words of each buffer it wrote and the count of non-zero bytes in
    the whole of it; the host gives the first 8 or 16 words. So the four words are
    compared, and then the two counts of non-zero bytes -- which is the stronger
    half of this, because the device's count covers the whole buffer: when it
    equals the count in the host's printed words, no byte outside them differs,
    and the row is an agreement about 4096 bytes rather than about sixteen.
    """
    dev_w = {i: b for i, b in dev["bufs"].items()}
    host_w = {i: b for i, b in host.items() if b["written"]}
    # The test prints a line per buffer it found non-zero bytes in, and one
    # sentence when it found none in any of them. Neither present means its
    # output was cut out of the log, which is not the same thing as a kernel that
    # wrote nothing -- and taking it for one would be the easiest false pass
    # here, since the two read identically in every other respect.
    if not dev_w and not dev["nothing"]:
        return "nolines", ("no buffer line and no \"nothing was written\" line: the test's "
                           "own account of what it found is not in the log")
    if dev_w and dev["nothing"]:
        return "nolines", ("the log says both that buffer(s) %s hold non-zero bytes and that "
                           "nothing was written to either buffer" % buflist(dev_w))
    if not dev_w and not host_w:
        return "silent", "neither side wrote a byte"
    if not dev_w or not host_w:
        return "onesided", ("the device wrote buffer(s) %s and softpipe %s"
                            % (buflist(dev_w), buflist(host_w)))
    if set(dev_w) != set(host_w):
        return "differ", ("the device wrote buffer(s) %s and softpipe %s"
                          % (buflist(dev_w), buflist(host_w)))
    notes, shown, accounted, blind = [], [], True, True
    for i in sorted(dev_w):
        theirs = dev_w[i]["words"]
        ours = host_w[i]["words"][:len(theirs)]
        if len(ours) < len(theirs):
            return "hostfail", "air_run printed %d words of buffer %d" % (len(ours), i)
        if ours != theirs:
            return "differ", ("buffer %d: softpipe %s, the Adreno %s"
                              % (i, " ".join("%08x" % w for w in ours),
                                 " ".join("%08x" % w for w in theirs)))
        if any(theirs):
            blind = False
        shown.append("buffer %d of %d bytes: %s"
                     % (i, dev_w[i]["size"], " ".join("%08x" % w for w in theirs)))
        hn = nonzero_bytes(host_w[i]["words"])
        if hn > dev_w[i]["nonzero"]:
            # softpipe's prefix alone holds more non-zero bytes than the device
            # has in the whole buffer: the four words agree and the buffers do
            # not, which is exactly the kind of pass worth refusing.
            return "differ", ("buffer %d: softpipe's first %d words hold %d non-zero bytes "
                              "and the whole buffer holds %d on the device"
                              % (i, len(host_w[i]["words"]), hn, dev_w[i]["nonzero"]))
        if hn < dev_w[i]["nonzero"]:
            accounted = False
            notes.append("buffer %d: %d non-zero bytes on the device, %d in the words "
                         "softpipe printed" % (i, dev_w[i]["nonzero"], hn))
    if blind:
        # Both sides wrote, but not where either side prints: comparing four
        # zero words against four zero words compares nothing.
        return "blind", ("buffer(s) %s were written on both sides, and the first four words "
                         "of each are zero on both sides" % buflist(dev_w))
    both = ", ".join(shown) + ", the same on both sides"
    if accounted:
        return "agree", both + ", and every non-zero byte the device counted is in them"
    return "agree-prefix", both + "; " + "; ".join(notes)


ROWS = [
    ("agree", "AGREE with softpipe, and the device's own count of non-zero bytes says "
              "the whole buffer agrees"),
    ("agree-prefix", "agree on the four words the device prints, which do not account for "
                     "every non-zero byte it counted"),
    ("differ", "DIFFER from softpipe"),
    ("onesided", "wrote on one side and not the other -- a DIFFERENCE, not a silence"),
    ("silent", "wrote nothing on either side: agreement, but it proves nothing -- a "
               "dispatch that did nothing at all reads the same"),
    ("nolines", "not compared: the log does not hold the test's own account of what it "
                "found in the buffers (the cut, most likely)"),
    ("blind", "wrote outside the words either side prints, so nothing was compared"),
    ("ntt", "not compared: softpipe refuses the shader (nir_to_tgsi). The device is the "
            "only reference these have"),
    ("hostfail", "not compared: the host reference could not be produced"),
    ("harness", "not compared: air_run did not run the device's grid on the device's input"),
    ("nolib", "not compared: the log's metallib is not under ios165/rootfs"),
    ("twice", "not compared: the log reports the kernel twice with different results"),
    ("refused", "not compared: the driver refused the dispatch, so no kernel ran -- not a "
                "result about the kernel"),
]
# What makes this check fail: the two sides disagreeing, and -- just as much --
# the check not having been made. A wrong IOS, an air_run that does not work or a
# grid that drifted would otherwise leave check.sh green while comparing nothing,
# which is the failure mode this whole suite is built against. nir_to_tgsi
# refusing a shader is not in here: that one is a known limit of softpipe's, it
# has its own row, and the device stays the only reference for those kernels.
BAD = ("differ", "onesided", "twice", "nolib", "harness", "hostfail")


def newest_log():
    """The newest log that holds a Metal compute run, or None."""
    files = [p for p in glob.glob(os.path.join(LOGS, "*-run.log")) if not os.path.islink(p)]
    for path in sorted(files, key=os.path.getmtime, reverse=True):
        with open(path, "rb") as f:
            if b"drvtest: dispatched " in f.read():
                return path
    return None


# --- the draw, compared the same way -------------------------------------------
#
# The device's draw test writes four expected words into its own source, which its
# own comment calls a weakness: if the shader's meaning ever changed, the constant
# would stay and the test would lie. So the reference is produced here instead, by
# running air_run on the same fragment shader, and the test's constants become a
# cross-check rather than the authority.
#
# Two differences from the dispatch comparison, both deliberate. The draw test
# fills its constant buffer with air_run's DEFAULT pattern rather than zeros, so
# air_run is run without --zero. And the attachment the device read is taken from
# its own line, because a shader that writes render target 2 and a harness that
# reads attachment 0 would be comparing two different pictures.
RE_D_NAME = re.compile(r"drvtest: draw: (\S+\.metallib) (\S+)\s*$", re.M)
RE_D_PIXEL = re.compile(r"drvtest: draw: the pixel at \((\d+), (\d+)\) of attachment "
                        r"(\d+) is ((?:\d+\s*){4})")
RE_D_STATUS = re.compile(r"drvtest: draw: status (\d+)")
RE_H_PIXEL = re.compile(r"the pixel at \((\d+), (\d+)\) of render target (\d+) came back")


# The three sentences air_run prints instead of a pixel, told apart.
RE_H_NOCOLOUR = re.compile(r"air_run: \S+ (?:writes no colour attachment|"
                           r"stores nothing to a colour attachment)")
RE_H_NOMS = re.compile(r"air_run: \S+ samples a multisample texture")
RE_H_TOOMANY = re.compile(r"air_run: (\d+) render targets, more than this can bind")


def host_pixel(lib, shader):
    """Which render target air_run drew into, and the four words it put there.

    (target, words, None, None) when there is a reference; (None, None, row,
    sentence) when there is not, and `row` is one of the named reasons of
    DRAW_NOREF so that the reasons can be counted apart instead of read.

    air_run prints exactly one pixel, and WHICH one is its decision rather than
    something to search for: `read_rt` in tools/mesa/air/air_run.c is the lowest
    colour attachment the shader declares, because a shader that writes only
    DATA2 leaves target 0 holding the clear colour and that number looks exactly
    like a result. The device prints every attachment of its render pass and
    leaves the choice to the reference. So the reference states its choice and
    the device's line is indexed with it.

    The version of this that produced 816462e took the target as an argument and
    was called once per attachment the device printed, keeping the LAST refusal:
    a shader writing target 1 therefore came out as "air_run printed no pixel for
    render target 0", which is a sentence about this function and about nothing
    else. 64 of that sweep's 65 uncompared rows said it.
    """
    try:
        out = subprocess.run([AIR_RUN, lib, shader], capture_output=True, text=True,
                             timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, None, "hostfail", "air_run did not finish in %d s" % TIMEOUT
    except OSError as e:
        # air_run half-written by a ninja running beside this is the ordinary way
        # to meet it, and a traceback out of the middle of a sweep loses the rows
        # already done. It is a host failure, which is a row, and the row is BAD.
        return None, None, "hostfail", "air_run could not be run: %s" % e
    text = out.stdout + out.stderr
    m = RE_H_NTT.search(text)
    if m:
        return None, None, "ntt", "softpipe refuses this shader: " + m.group(1).strip()
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = RE_H_PIXEL.search(line)
        if not m:
            continue
        # air_run prints a pixel as four bare words, one per line -- unlike a buffer,
        # which it prints as "[ 0]  2147483647  0x7fffffff". Two parser mistakes of mine
        # lived here, and both read as a broken reference rather than a broken parser:
        # it first looked for hexadecimal, which a pixel does not have, and then only for
        # integers -- while a FLOAT render target is printed as "2.10170319e-18". The
        # device reads every attachment as RGBA32Uint and prints raw words, so the two
        # sides hold the same bits in different clothing, and the float is converted to
        # its bit pattern rather than dropped. %.9g round-trips float32 exactly, so the
        # conversion is lossless.
        # ... and a float that happens to be whole is printed WITHOUT a decimal
        # point, so "how it looks" is not enough to tell a float target from an
        # integer one. texturedFragment2DFloat samples texel (1,1) and air_run
        # prints 101, 201, 302, 400 while the device prints 1120534528 and the
        # rest -- the same four numbers, one side in decimal and the other in
        # bits -- and reading them as integers made two shaders of every sweep
        # "differ for a reason the orientation cannot explain". The format is not
        # guesswork either: air_run says it in the line after the pixel, "drew
        # 4x4 into r32g32b32a32_float", so the decision is read rather than
        # inferred from the shape of a digit.
        is_float = False
        for rest in lines[i + 1:i + 14]:
            f = re.match(r"air_run: drew \d+x\d+ into (\S+?),", rest)
            if f:
                is_float = "float" in f.group(1)
                break
        words = []
        for rest in lines[i + 1:i + 9]:
            w = re.match(r"\s+(-?[0-9][0-9a-fA-FxX.eE+-]*)\s*$", rest)
            if not w:
                break
            text_word = w.group(1)
            if re.fullmatch(r"\d+", text_word) and not is_float:
                words.append(int(text_word))
            else:
                try:
                    bits = struct.unpack("<I", struct.pack("<f", float(text_word)))[0]
                except (ValueError, struct.error):
                    break
                words.append(bits)
        if len(words) >= 4:
            return int(m.group(3)), words[:4], None, None
        return None, None, "hostfail", ("air_run printed %d words for render target %s"
                                        % (len(words), m.group(3)))
    # No pixel line at all. air_run says why in one sentence, and the three
    # reasons it has are three different statements about the shader: one that
    # writes no colour attachment wrote nothing anywhere (clearFragment00 is
    # `define void @clearFragment00(ptr) { ret void }`), one whose only output is
    # depth or stencil wrote somewhere this harness does not read, and one that
    # samples a multisample texture was not run at all rather than run against
    # the wrong texture. Collapsing them into "no reference" loses exactly the
    # part that says whether a reference is possible.
    if RE_H_NOCOLOUR.search(text):
        return None, None, "nocolour", (
            "air_run: " + RE_H_NOCOLOUR.search(text).group(0).split(None, 1)[1])
    if RE_H_NOMS.search(text):
        return None, None, "multisample", (
            "the shader samples a multisample texture and neither harness binds one")
    m = RE_H_TOOMANY.search(text)
    if m:
        return None, None, "hostfail", m.group(0)
    tail = [l.strip() for l in lines if l.strip()]
    return None, None, "hostfail", (tail[-1][:140] if tail else
                                    "air_run printed nothing at all")


def draw_section(path):
    """The draw rows. True when nothing in them failed; a log with no draw passes.

    The log is read here rather than handed in, the same way the dispatch side reads
    it, and in the status numbering it was written in (status_scheme)."""
    text = log_text(path)
    completed = status_scheme(path)[0]
    name = RE_D_NAME.search(text)
    status = RE_D_STATUS.search(text)
    pixel = RE_D_PIXEL.search(text)

    if not name and not status:
        return True
    print()
    if not name:
        print("metalcheck: a draw ran but did not name its shader, so no reference can be "
              "produced for it (an older build of the test)")
        return True
    lib, shader = name.group(1), name.group(2)
    print("metalcheck: the draw: %s of %s" % (shader, lib))
    if not status or status.group(1) != str(completed):
        print("    the command buffer did not complete (status %s, and completion is %d in this "
              "log), so there is nothing to compare"
              % (status.group(1) if status else "unknown", completed))
        return True
    if not pixel:
        # In --draw-all the test hands the readback to the sweep and returns before
        # printing this pixel (`if (draw_all) { draw_every(...); return; }`), while
        # the setup lines above -- including "draw: status N" -- are printed either
        # way. Calling that a hole in the log is this tool inventing a loss: all
        # five slices of 816462e said it, under a heading naming the one shader the
        # single draw would have used.
        if RE_DA.search(text) or RE_DA_TOTAL.search(text):
            print("    the single draw's own readback is not in a --draw-all run: the "
                  "test hands it to the sweep below, which reads every attachment")
        else:
            print("    it completed but printed no pixel, which is a hole in the log "
                  "rather than a result")
            if completed == NEW_SCHEME[2]:
                print("    (%s, and returns before its own readback)" % TEST_KEYS_ON_SIX)
        return True

    target = int(pixel.group(3))
    device = [int(w) for w in pixel.group(4).split()]
    path = os.path.join(IOS, "rootfs") + lib
    if not os.path.exists(path):
        print("    not compared: %s is not under ios165/rootfs" % lib)
        return True
    host_rt, host, _row, why = host_pixel(path, shader)
    if host is None:
        print("    not compared: %s" % why)
        print("    (the device drew %s into attachment %d)"
              % (" ".join("%u" % w for w in device), target))
        return True
    if host_rt != target:
        # The device's draw path prints one attachment and names it; air_run picks
        # the lowest the shader declares. The two agreeing on the index is part of
        # the comparison, not a precondition to be assumed.
        print("    not compared: air_run drew into render target %d and the device read "
              "attachment %d, which are two different pictures" % (host_rt, target))
        return True
    print("    device   %s" % " ".join("%u" % w for w in device))
    print("    softpipe %s" % " ".join("%u" % w for w in host))
    if device == host:
        print("    AGREE, on the pixel at (0, 0) of attachment %d -- two backends over the "
              "same front end of ours, so a mistranslation would be on both sides" % target)
        return True
    print("    DIFFER, and a different picture is a failure rather than a variation")
    return False


# --- the whole-library draw sweep ----------------------------------------------
#
# --draw-all draws every fragment shader of a library through the driver and prints
# one line each. Each of those shaders has a reference: air_run draws them all on
# softpipe. So the sweep can be compared shader by shader -- with one correction
# that is not optional.
#
# A shader whose result depends on a varying or on its position is kept apart from
# one whose result does not, and the reason has changed twice. It was read as the two
# back ends orienting the picture differently (docs/metal.md 10.26): the device
# printed at pixel (0,0) the texel softpipe printed at (0,3). That was wrong, and
# 10.32 is how: every varying was FLAT on the Adreno, because finalize_for_gallium
# called nir_lower_io without nir_lower_io_use_interpolated_input_intrinsics while
# softpipe inferred smooth interpolation from the variable. A flat varying puts the
# provoking vertex's texel under pixel (0,0) -- the same four numbers a mirrored
# picture would put there -- and one pixel cannot tell those apart. With the flag in,
# all sixteen pixels of that draw match the texture texel for texel
# (logs/090905-run.log).
#
# So these rows are no longer expected to differ at all. They are still counted
# separately, for two reasons: a log made before that fix still has them, and the
# orientation question itself is not closed -- Metal requires clip y = +1 in memory
# row 0 and our viewport puts clip y = -1 there (10.28), a one-line change that is
# written down and deliberately not landed. When it lands, these are the rows that
# move, and a row that can move for a known reason should not sit inside a count of
# agreements.
# The trailing group holds EVERY " tN w w w w" the line carries, and getting that
# wrong was the whole of 816462e's 65 uncompared rows. The pattern there ended in
# `((?: t\d (?:\d+ ?){4})*)`, whose inner `\d+ ?` eats the space in front of the
# next "t"; the outer `*` then stops after one repetition, and -- because the group
# is the last thing in the pattern -- nothing afterwards fails, so nothing forces
# the engine to backtrack and the match is accepted with t0 alone. The device had
# been printing all three attachments since 816462e and this read one of them, which
# is the same mistake that commit's own message reports fixing on the device side.
# Hence `\s+` before each "t" and `(?:\s+\d+)`, which cannot cross it.
RE_DA = re.compile(r"drvtest: drawall (\S+)\s+status (\d+)((?:\s+t\d(?:\s+\d+){4})*)")
RE_DA_T = re.compile(r"t(\d)((?:\s+\d+){4})")
# A second reader of the same line, written to share nothing with the first: the
# whole rest of the line, and the "tN" markers counted in it. The two must agree
# on how many attachments the line carries. That disagreement is the only thing
# that would have caught 816462e's mistake on its first row -- the sweep was
# internally consistent while reading one attachment of three, and nothing in the
# numbers it printed could show it. One regex for the structure and one for the
# count, and a mismatch is this tool's failure and said so.
RE_DA_LINE = re.compile(r"drvtest: drawall (\S+)\s+status (\d+)(.*)$", re.M)
RE_DA_LOOSE = re.compile(r"(?<![0-9A-Za-z_])t\d(?=\s|$)")
# The two outcomes that are NOT a "status" line, and so do not match RE_DA at all.
# Both were invisible to this tool until they were looked for: `no pipeline` has
# always been printed and has always been zero, and `no texture` arrived with the
# per-shader texture binding. A row the sweep printed and this did not read is the
# failure mode the whole ledger exists against -- the sweep would have said 271
# shaders and this would have counted 265 without a word.
RE_DA_NOPIPE = re.compile(r"drvtest: drawall (\S+)\s+no pipeline: (.*)$", re.M)
RE_DA_NOTEX = re.compile(r"drvtest: drawall (\S+)\s+no texture: (.*)$", re.M)
# What was actually bound, at the end of a drawn row: "tex 0=2:125 1=7:123", as
# slot=MTLTextureType:MTLPixelFormat. A row of a table is a measurement only if
# what it was measured against is in it.
RE_DA_TEX = re.compile(r"(?<![0-9A-Za-z_])tex((?:\s+\d+=\d+:\d+)+)")
RE_DA_TEX_ONE = re.compile(r"(\d+)=(\d+):(\d+)")
# The sweep's own summary, read as a LIST of outcomes rather than as a fixed
# number of them. It had three, it has four, and the fourth appeared while this
# tool was being written -- so an unknown phrase with a non-zero count beside it
# is a failure here and not something to skip past. The parenthesis differs
# between the two shapes too (the newer one names how many shaders asked for a
# texture and got it), so everything after the list is optional.
RE_DA_TOTAL = re.compile(r"drvtest: drawall: ([^(\n]+?)\s*"
                         r"\(of the (\d+) fragment shaders from index (\d+)"
                         r"(?:; (\d+) declared a texture and were bound what they "
                         r"declared)?\)")
RE_DA_ITEM = re.compile(r"^(\d+) (.+)$")
# A third reader, dumber than both: every per-shader line of the sweep, whatever
# it goes on to say. The summary line is "drvtest: drawall: ..." with a colon, so
# it is not one of these.
#
# It exists because of a hole the other checks did not close. Blinded to the "no
# texture" rows, this tool read 13 of the sweep's 19 shaders, found its own ledger
# six short, and blamed the loader's cut -- the arithmetic added up and the
# sentence was false. A cut REMOVES lines; an outcome nobody taught this to read
# leaves the line sitting in the log. Counting the lines tells those two apart,
# and nothing else here does.
RE_DA_ANY = re.compile(r"drvtest: drawall (\S+)")
# phrase -> which of this section's columns the sweep is counting
DA_OUTCOMES = {
    "drew": "drew",
    "refused at the draw": "refused",
    "got no pipeline": "nopipe",
    "could not be given the textures they declare": "notex",
}


# MTLTextureType and MTLPixelFormat as names, read out of Apple's own headers --
# the ones userland/build.sh builds the test against, or the copy in xnu-deps.
# Not a table written here from memory: the numbers in the log are whatever that
# SDK says they are, and a name this invented would be a caption on somebody
# else's picture. Where no header can be read the number is printed bare.
_MTL_ENUMS = {}


def mtl_name(kind, value):
    """"MTLTextureType2D" for (texture, 2), or "2" when no header says so."""
    if not _MTL_ENUMS:
        sdks = [os.path.expanduser("~/theos/sdks/iPhoneOS16.5.sdk"),
                os.path.join(ROOT, "xnu-deps/sdk/iPhoneOS16.5.xnu.sdk")]
        if os.environ.get("THEOS"):
            sdks.insert(0, os.path.join(os.environ["THEOS"], "sdks/iPhoneOS16.5.sdk"))
        for which, header in (("texture", "MTLTexture.h"), ("format", "MTLPixelFormat.h")):
            _MTL_ENUMS[which] = {}
            for sdk in sdks:
                path = os.path.join(
                    sdk, "System/Library/Frameworks/Metal.framework/Headers", header)
                if not os.path.exists(path):
                    continue
                try:
                    text = open(path, errors="replace").read()
                except OSError:
                    continue
                for name, num in re.findall(
                        r"^\s+(MTL(?:TextureType|PixelFormat)\w+)\s*=\s*(\d+)\s*,",
                        text, re.M):
                    _MTL_ENUMS[which].setdefault(int(num), name)
                break
    return _MTL_ENUMS.get(kind, {}).get(value, str(value))


def tex_bound(rest):
    """The "tex ..." suffix of a drawn row as [(slot, type name, format name)]."""
    m = RE_DA_TEX.search(rest or "")
    if not m:
        return []
    return [(int(slot), mtl_name("texture", int(t)), mtl_name("format", int(f)))
            for slot, t, f in RE_DA_TEX_ONE.findall(m.group(1))]


def position_dependent(lib, name):
    """Does this shader read a varying or its own position? (air2nir --ll)"""
    try:
        out = subprocess.run([AIR2NIR, "--ll", lib, name], capture_output=True,
                             timeout=TIMEOUT).stdout.decode(errors="replace")
    except (subprocess.TimeoutExpired, OSError):
        return None
    return ('air.fragment_input' in out or 'air.position' in out
            or 'air.vertex_output' in out)


def sweep_library(path):
    """The library the sweep drew, as the test's own line names it, under ios165/rootfs."""
    raw = open(path, "rb").read()
    text = raw.decode("utf-8", errors="replace").replace("\x00", "")
    m = re.search(r"drvtest: (\S+\.metallib), \d+ bytes", text)
    if not m:
        return None
    return os.path.join(IOS, "rootfs") + m.group(1)


# The sweep's own reason table. Ten reasons were already named on the dispatch
# side (ROWS) and the sweep had none: it collected a sentence per shader and
# printed four of them, so "65 could not be compared" was a number with no parts.
# Each of these is a different statement, and the point of keeping them apart is
# that some are this harness's business and some are not.
DRAW_NOREF = [
    ("nopixel", "the log carries no pixel for it, which is a hole in the log and not a "
                "result (the loader drops the middle of a long log)"),
    ("twice", "the log reports it twice with two different pictures, which is not "
              "something to pick between"),
    ("nocolour", "the shader writes no colour attachment this harness reads, so there is "
                 "nothing to compare -- and the device's attachments all hold the clear "
                 "value, which agrees with that and proves nothing"),
    ("multisample", "the shader samples a multisample texture and neither harness binds "
                    "one, so it is not run rather than run against the wrong texture"),
    ("beyond", "air_run drew into a render target the device's render pass does not bind, "
               "so the device never held this shader's output"),
    ("ntt", "softpipe refuses the shader (nir_to_tgsi/tgsi_ureg). That is softpipe's "
            "limit; the device is the only reference these have"),
    ("notex", "the driver could not be given the texture the shader declares, so it was "
              "not drawn rather than drawn against the wrong texture -- the sentence is "
              "the measurement"),
    ("nopipe", "the driver made no pipeline for it, so nothing was drawn and this is not "
               "a result about the shader"),
    ("hostfail", "the host reference could not be produced"),
]
# Of these, `twice` and `hostfail` are something going wrong here. The rest are
# measurements: a property of the shader, of the device test's render pass, or of
# softpipe, each of which stays until something is built. `nopixel` is the
# loader's, not ours -- it keeps the first 256 KB of a boot's log and the last
# 1 MB -- and the dispatch side's equivalent row (`nolines`) is kept out of BAD
# for the same reason: a cut log must not turn check.sh red.
DRAW_BAD = ("twice", "hostfail")


def drawall_section(path, library_hint):
    """The sweep's rows. True when nothing in them failed; no sweep in the log passes."""
    text = log_text(path)
    completed = status_scheme(path)[0]
    status_rows = RE_DA.findall(text)
    notex_rows = RE_DA_NOTEX.findall(text)
    nopipe_rows = RE_DA_NOPIPE.findall(text)
    if not status_rows and not notex_rows and not nopipe_rows:
        return True

    lib = library_hint
    if lib is None or not os.path.exists(lib):
        print("metalcheck: the sweep's library is not under ios165/rootfs, so no reference "
              "can be produced for it")
        return True

    # The loose reader of each line, and the whole rest of it (for the "tex" suffix).
    loose, rest_of = {}, {}
    for name, status, rest in RE_DA_LINE.findall(text):
        loose.setdefault(name, len(RE_DA_LOOSE.findall(rest)))
        rest_of.setdefault(name, rest)

    # One shader is one row, keyed on the name, over ALL THREE kinds of line the
    # sweep prints. The loader's heartbeat reprints the console's tail, so a line
    # can appear twice: three of the five slices of 816462e held one such echo each
    # and the sweep counted 274 rows for 271 shaders. Two copies that say the same
    # thing are one row; two that disagree are a row of their own.
    order, byname, twice = [], {}, []
    echoes = 0

    def add(name, key):
        nonlocal echoes
        if name not in byname:
            order.append(name)
            byname[name] = key
        elif byname[name] == key:
            echoes += 1
        elif name not in twice:
            twice.append(name)

    for name, status, pixel in status_rows:
        add(name, ("status", status, " ".join((pixel or "").split())))
    for name, why in notex_rows:
        add(name, ("notex", why.strip()))
    for name, why in nopipe_rows:
        add(name, ("nopipe", why.strip()))

    agree, differ, flip, refused = [], [], [], []
    mismatched = []
    noref = {key: [] for key, _ in DRAW_NOREF}
    drew_rows = 0
    # How much a comparison of one attachment can say. These shaders write a
    # constant out of a uniform into some of three targets, and two targets often
    # hold the same constant -- so for some rows the right attachment and the wrong
    # one carry the same four words, and such a row would agree even if the two
    # sides' indices were off. Counted rather than assumed away.
    flat = 0
    # Which attachment each comparison was about. Printed because it is the number
    # that was silently 0 for all of them, and a column of zeros here is the shape
    # of this going wrong again.
    on_target = {}
    # What the device bound, for the rows that declare a texture at all.
    bound = {}
    with_tex_rows = set()
    for name in order:
        key = byname[name]
        if name in twice:
            noref["twice"].append((name, "two different outcomes under one name"))
            continue
        if key[0] in ("notex", "nopipe"):
            noref[key[0]].append((name, key[1]))
            continue
        status, pixel = key[1], key[2]
        if status != str(completed):
            refused.append(name)
            continue
        drew_rows += 1
        # counted for every DRAWN row, compared or not, because that is what the
        # summary's "declared a texture and were bound what they declared" counts
        if tex_bound(rest_of.get(name, "")):
            with_tex_rows.add(name)
        # every attachment the device read, by target
        got = {int(t): [int(w) for w in words.split()]
               for t, words in RE_DA_T.findall(pixel or "")}
        if loose.get(name, len(got)) != len(got):
            mismatched.append((name, loose[name], len(got)))
        if not got:
            noref["nopixel"].append(
                (name, "completed (status %d) and no attachment on the line" % completed))
            continue
        # Which attachment to compare is the reference's answer: air_run prints the
        # lowest colour attachment the shader declares and says which one that is.
        target, host, row, why = host_pixel(lib, name)
        if host is None:
            if row == "nocolour":
                why += "; the device's attachment(s) %s hold %s" % (
                    buflist(got), "the clear value"
                    if not any(any(w) for w in got.values()) else "non-zero bytes")
            noref[row].append((name, why))
            continue
        if target not in got:
            noref["beyond"].append(
                (name, "air_run drew into render target %d and the line carries "
                       "attachment(s) %s" % (target, buflist(got))))
            continue
        if len({tuple(w) for w in got.values()}) == 1:
            flat += 1
        on_target[target] = on_target.get(target, 0) + 1
        tex = tex_bound(rest_of.get(name, ""))
        if tex:
            bound[name] = (target, tex)
        device = got[target]
        if device == host:
            agree.append(name)
        elif position_dependent(lib, name):
            flip.append(name)
        else:
            differ.append((name, target, device, host))

    # --- the sweep's own account of the same run -------------------------------
    #
    # Read as a LIST of outcomes and not as a fixed number of them. It had three,
    # it has four, and the fourth arrived while this was being written: six shaders
    # the driver will not bind a multisample texture for now print "no texture"
    # instead of a status, so a tool counting only status lines would have read 265
    # rows where the sweep saw 271 and said nothing. An outcome this does not know
    # is therefore a failure here, not a column to leave out.
    total = RE_DA_TOTAL.search(text)
    said, unknown, shown_total = {}, [], []
    library_total, slice_from, with_textures = None, None, None
    if total:
        for item in total.group(1).split(", "):
            m = RE_DA_ITEM.match(item.strip())
            if not m:
                unknown.append(item.strip())
                continue
            n, phrase = int(m.group(1)), m.group(2).strip()
            shown_total.append("%d %s" % (n, phrase))
            col = DA_OUTCOMES.get(phrase)
            if col is None:
                unknown.append(item.strip())
            else:
                said[col] = said.get(col, 0) + n
        library_total, slice_from = int(total.group(2)), int(total.group(3))
        with_textures = int(total.group(4)) if total.group(4) else None

    # Lines of the sweep that none of the readers above understood.
    unread = [n for n in dict.fromkeys(RE_DA_ANY.findall(text)) if n not in byname]

    mine = {"drew": drew_rows, "refused": len(refused),
            "nopipe": len(noref["nopipe"]), "notex": len(noref["notex"])}
    # What the sweep says it was asked for, which is the number the ledger has to
    # add to -- not the rows that happen to be in the log. A cut log holds the
    # summary (it is the last line) and not every row above it, and the difference
    # is a loss that must be named rather than quietly divided out.
    asked = sum(said.values()) if said else len(order)
    lost = asked - len(order)

    uncompared = sum(len(v) for v in noref.values())
    print()
    head = "metalcheck: the draw sweep, %d shaders the sweep says it was asked to draw" % asked
    if echoes:
        head += (" (%d line%s of the %d was the loader's heartbeat reprinting an earlier "
                 "one)" % (echoes, "" if echoes == 1 else "s",
                           len(status_rows) + len(notex_rows) + len(nopipe_rows)))
    if shown_total:
        head += " (its own count: %s" % ", ".join(shown_total)
        if library_total is not None:
            head += "; the library holds %d fragment shaders and this slice starts at %d" % (
                library_total, slice_from)
        if with_textures is not None:
            head += "; %d declared a texture and were bound what they declared" % with_textures
        head += ")"
    print(head)
    print("  %3d AGREE with softpipe on the pixel at (0, 0)%s" % (
        len(agree), "" if not on_target else ", compared on attachment " + ", ".join(
            "%d for %d of them" % (t, on_target[t]) for t in sorted(on_target))))
    print("  %3d DIFFER, and they read no varying and no position, so the orientation "
          "cannot explain it" % len(differ))
    print("  %3d differ but read a varying or a position: the two backends orient the "
          "picture differently (10.26, read again in 10.32), so this is not counted "
          "either way" % len(flip))
    print("  %3d the driver did not finish (status not %d)" % (len(refused), completed))
    print("  %3d could not be compared:" % uncompared)
    for key, what in DRAW_NOREF:
        if noref[key]:
            print("      %3d  %s" % (len(noref[key]), what))
    if noref["nopixel"] and completed == NEW_SCHEME[2]:
        # Where every drawn row has no readback, the driver's numbers moved and the test's
        # did not; this is the one row whose cause is in another file, so it says which.
        print("      (%s)" % TEST_KEYS_ON_SIX)
    if lost:
        print("  %3d the sweep counted and this log does not hold: the loader keeps the "
              "first 256 KB of a boot's log and the last 1 MB, and the summary is the "
              "last line, so it can outlive the rows it counts" % lost)
    for name, target, device, host in differ[:8]:
        print("      %-34s target %d: device %s | softpipe %s" % (
            name, target, " ".join("%u" % w for w in device),
            " ".join("%u" % w for w in host)))
    # Every uncompared row by name, not the first four: the reason a row is not
    # compared is the only thing it has to say, and a capped list hid 61 of 65.
    for key, _ in DRAW_NOREF:
        for name, why in sorted(noref[key]):
            print("      %-11s %-34s %s" % (key, name, why))
    if agree or differ or flip:
        print("  (of the %d compared, %d had all of the device's attachments holding the "
              "same four words, so for those the attachment this picked and the ones it "
              "did not would read alike; the other %d would have shown a DIFFER had the "
              "two sides' attachment indices not lined up)"
              % (len(agree) + len(differ) + len(flip), flat,
                 len(agree) + len(differ) + len(flip) - flat))
    # What each compared shader that declares a texture was bound. The 256 that
    # declare none are bound the sweep's own texture and print no suffix, so the
    # absence of a line here is itself the statement. air_run binds its OWN
    # texture and does not print what it bound, so the two sides' texture is not
    # cross-checked by this -- said out loud, because a row that names one side's
    # input and not the other's is half a measurement.
    if bound:
        print("  the %d compared shader%s that declare a texture, and what the device "
              "bound (air_run does not say what it bound, so this is one side only):"
              % (len(bound), "" if len(bound) == 1 else "s"))
        for name in sorted(bound):
            target, tex = bound[name]
            print("      %-34s attachment %d, %s%s" % (
                name, target,
                ", ".join("slot %d %s of %s" % t for t in tex),
                "" if name in agree else "  [NOT an agreement]"))

    ok = True
    if unread:
        print("metalcheck: %d line%s of the sweep this did not read (%s%s): they are in "
              "the log, so they are not the loader's cut -- an outcome of the sweep has "
              "no reason here and every count above is short by them"
              % (len(unread), "" if len(unread) == 1 else "s",
                 " ".join(unread[:6]), " ..." if len(unread) > 6 else ""),
              file=sys.stderr)
        ok = False
    if unknown:
        print("metalcheck: the sweep's summary names %d outcome%s this does not know (%s), "
              "so the ledger below cannot be checked against it"
              % (len(unknown), "" if len(unknown) == 1 else "s", "; ".join(unknown)),
              file=sys.stderr)
        ok = False
    if said and not twice:
        # Every row of the log must be an outcome the summary also counted, and no
        # more of them than the summary counted. With a `twice` row in the log one
        # shader cannot be attributed to a column, so this is not attempted then
        # rather than reported wrongly.
        for col, n in sorted(said.items()):
            if mine[col] > n:
                print("metalcheck: the log holds %d %s rows and the sweep counted %d"
                      % (mine[col], col, n), file=sys.stderr)
                ok = False
        if with_textures is not None and len(with_tex_rows) > with_textures:
            # The summary counts the shaders that declared a texture and were given
            # it, drawn or not; a "tex" suffix is printed only on a drawn row. So
            # the rows may be fewer and must never be more. Checked because this is
            # the fifth number of a line that had three of them a day ago.
            print("metalcheck: %d drawn rows name a bound texture and the sweep counted "
                  "%d shaders that declared one"
                  % (len(with_tex_rows), with_textures), file=sys.stderr)
            ok = False
        if sum(said.values()) - sum(mine.values()) != lost:
            print("metalcheck: the sweep counted %d rows, this read %d, and %d are "
                  "accounted for as lost" % (sum(said.values()), sum(mine.values()), lost),
                  file=sys.stderr)
            ok = False
    if lost < 0:
        print("metalcheck: this read %d rows and the sweep says it drew %d"
              % (len(order), asked), file=sys.stderr)
        ok = False
    if len(agree) + len(differ) + len(flip) + len(refused) + uncompared + lost != asked:
        print("metalcheck: the sweep's rows do not add up", file=sys.stderr)
        ok = False
    if mismatched:
        name, n_loose, structured = mismatched[0]
        print("metalcheck: the line for %s carries %d \"tN\" markers and RE_DA read %d of "
              "them (%d such line%s): the attachments were not read, so every row above "
              "is about a picture this did not look at"
              % (name, n_loose, structured, len(mismatched),
                 "" if len(mismatched) == 1 else "s"), file=sys.stderr)
        ok = False
    bad = [k for k in DRAW_BAD if noref[k]]
    return ok and not differ and not bad


def main(argv):
    want_list = "--list" in argv
    log = None
    if "--log" in argv:
        log = argv[argv.index("--log") + 1]
    elif "--newest" in argv:
        log = newest_log()
        if log is None:
            # A run on this machine has no device log, and that is not a failure:
            # this check is about a boot that happened, not about the frontend.
            print("metalcheck: no device log holds a Metal compute run, so there is "
                  "nothing to check against")
            return 0
    if log is None:
        print("usage: metalcheck.py --log RUN.log [--list]\n"
              "       metalcheck.py --newest [--list]", file=sys.stderr)
        return 1
    if not os.path.exists(log):
        print("metalcheck: no such log %s" % log, file=sys.stderr)
        return 1
    if not os.path.exists(AIR_RUN):
        print("metalcheck: run tools/air/mk-host.sh first (%s)" % AIR_RUN, file=sys.stderr)
        return 1

    status_completed, _, scheme_note, scheme_brief = status_scheme(log)
    print(scheme_note)
    ks, orphans, dropped = read_log(log)
    if not ks:
        # A boot that only drew has no compute line, and the draw is worth checking on
        # its own: the first triangle was run that way deliberately, so that "did it
        # draw" could not be answered out of a log whose middle had been dropped.
        ok = draw_section(log) and drawall_section(log, sweep_library(log))
        print(scheme_brief)
        return 0 if ok else 1

    compiled = sorted(n for n, k in ks.items() if k["compiled"])
    crefused = sorted(n for n, k in ks.items() if k["crefused"] and not k["compiled"])
    dispatched = sorted(n for n, k in ks.items() if k["distinct"])
    nodispatch = [n for n in compiled if not ks[n]["distinct"]]
    finished = [n for n in dispatched
                if all(r["status"] == status_completed for r in ks[n]["distinct"])]

    # Every dispatch ends in exactly one row.
    rows = {key: [] for key, _ in ROWS}
    cache = {}
    for name in dispatched:
        k = ks[name]
        if len(k["distinct"]) > 1:
            rows["twice"].append((name, "%d different outcomes for one name"
                                  % len(k["distinct"])))
            continue
        dev = k["distinct"][0]
        if dev["status"] != status_completed:
            rows["refused"].append((name, "status %d: %s" % (dev["status"], dev["error"])))
            continue
        libs = sorted(l for l in k["libs"] if l)
        if len(libs) != 1:
            rows["nolib"].append((name, "the log names %d libraries for it" % len(libs)))
            continue
        host_lib = os.path.join(IOS, "rootfs", libs[0].lstrip("/"))
        if not os.path.exists(host_lib):
            rows["nolib"].append((name, "missing %s" % host_lib))
            continue
        if (host_lib, name) not in cache:
            cache[(host_lib, name)] = host_run(host_lib, name)
        res, row, why = cache[(host_lib, name)]
        if res is None:
            rows[row].append((name, why))
            continue
        row, why = compare(dev, res)
        rows[row].append((name, why))

    if want_list:
        for key, _ in ROWS:
            for name, why in sorted(rows[key]):
                print("  %-12s %-40s %s" % (key, name, why))
        print()

    print("metalcheck: %s, %d kernels the driver was asked to compile"
          % (os.path.basename(log), len(compiled) + len(crefused)))
    print("  %3d  compiled by the compiler library" % len(compiled))
    print("  %3d  refused by it, with a reason" % len(crefused))
    print("  %3d  dispatched" % len(dispatched))
    print("  %3d  of those the GPU finished (status %d)" % (len(finished), status_completed))
    print("  %3d  the driver refused before the GPU saw them" % len(rows["refused"]))
    print("  %3d  compiled but never dispatched (the test dispatches one function of the "
          "library)" % len(nodispatch))
    print()
    print("  of the %d dispatches, against softpipe on the same input (zeros) and the same "
          "grid (%s):" % (len(dispatched), GRID))
    for key, what in ROWS:
        print("  %3d  %s" % (len(rows[key]), what))
    # The rows that make this fail are printed whether or not --list was asked
    # for, because check.sh reads the tail of this and nothing else. Capped: a
    # wrong IOS puts every kernel in one of them and the summary matters more.
    shown = [(k, n, w) for k in BAD for n, w in sorted(rows[k])]
    for key, name, why in shown[:6]:
        print("       %-12s %-40s %s" % (key.upper(), name, why))
    if len(shown) > 6:
        print("       ... and %d more; --list prints every row" % (len(shown) - 6))

    if orphans or dropped:
        print()
        print("  (%d buffer line%s with no dispatch line above, %d cut%s in the log: the "
              "loader keeps the first 256 KB and the last 1 MB and its heartbeat reprints "
              "the console's tail, so a stretch can appear twice and nothing is read from "
              "the order of the lines)"
              % (orphans, "" if orphans == 1 else "s", dropped,
                 "" if dropped == 1 else "s"))

    # The job list is a second source for what was supposed to run: a process
    # that died before printing anything leaves no line at all, and nothing in
    # the log can show that. Only named for a log that is a run of this list --
    # over half of it present -- because another job list sharing one kernel
    # with it would otherwise report the other eight as lost.
    def kernels_of(path):
        out = []
        for line in open(path, errors="replace").read().splitlines():
            f = line.split()
            # `metal_drv_test [flags] <metallib> <kernel>`, and only that shape:
            # the list also holds lines that name no kernel at all (--draw, which
            # asks the library for a pipeline rather than a compute function),
            # and reading the metallib's path off the end of one as a kernel name
            # would report it missing from every log forever.
            if (len(f) >= 3 and f[0].endswith("/metal_drv_test")
                    and f[-2].endswith(".metallib") and "/" not in f[-1]):
                out.append(f[-1])
        return out

    # The list this log overlaps most, and among equals the SHORTEST -- a log of nine
    # kernels is a run of the nine-kernel list, not of the thirty-three-kernel one
    # that contains them, and the first version of this chose the latter and then
    # announced that it was looking at some other list's run.
    want, jobs_name, best = [], None, None
    for jobs_path in sorted(glob.glob(JOB_GLOB)):
        names = kernels_of(jobs_path)
        if not names:
            continue
        score = (len([n for n in names if n in ks]), -len(names))
        if best is None or score > best:
            want, jobs_name, best = names, os.path.basename(jobs_path), score
    if want:
        absent = [n for n in want if n not in ks]
        here = len(want) - len(absent)
        is_are = "is" if here == 1 else "are"
        if want and len(absent) * 2 < len(want):
            print("  (%d of the %d kernels of %s %s in this log%s)"
                  % (here, len(want), jobs_name, is_are,
                     ", and these are not, so their processes printed nothing at all: "
                     + " ".join(absent) if absent else ""))
        elif want:
            print("  (%d of the %d kernels of %s %s in this log, so this "
                  "is a run of some other job list)"
                  % (here, len(want), jobs_name, is_are))

    ok = True
    if len(compiled) + len(crefused) != len(ks):
        print("metalcheck: %d compiled and %d refused, and the log names %d kernels"
              % (len(compiled), len(crefused), len(ks)), file=sys.stderr)
        ok = False
    if len(finished) + len(rows["refused"]) != len(dispatched):
        print("metalcheck: %d finished and %d were refused, and %d were dispatched"
              % (len(finished), len(rows["refused"]), len(dispatched)), file=sys.stderr)
        ok = False
    if len(dispatched) + len(nodispatch) != len(compiled):
        print("metalcheck: %d dispatched and %d not, and %d compiled"
              % (len(dispatched), len(nodispatch), len(compiled)), file=sys.stderr)
        ok = False
    total = sum(len(v) for v in rows.values())
    if total != len(dispatched):
        print("metalcheck: the comparison rows add to %d and %d were dispatched"
              % (total, len(dispatched)), file=sys.stderr)
        ok = False
    if not ok:
        print("metalcheck: the rows do not add up, so read none of the numbers above",
              file=sys.stderr)
        return 1
    strong = len(rows["agree"]) + len(rows["agree-prefix"])
    print("metalcheck: the comparison rows add to %d, which is every dispatch. %s two "
          "backends over the same frontend of ours, so a mistranslation of Apple's bitcode "
          "would be on both sides; and the input is zeros, which is an honest input only for "
          "a kernel that initialises or reduces."
          % (total, "No row compares" if strong == 0 else
             "The 1 strong row compares" if strong == 1 else
             "The %d strong rows compare" % strong))
    bad = any(rows[k] for k in BAD)
    # `log`, and it used to be `path` -- which this function never binds: it was
    # the last job list of the loop above, leaked out of it. A log holding both a
    # dispatch and a draw therefore had its draw and its sweep read out of
    # userland/rc.jobs.* , where there is no draw, and both sections returned
    # "nothing here" without a word. Nothing in this project's logs took that
    # branch, which is the only reason it was never seen.
    ok_draw = draw_section(log)
    ok_sweep = drawall_section(log, sweep_library(log))
    print(scheme_brief)
    return 0 if ok_draw and ok_sweep and not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
