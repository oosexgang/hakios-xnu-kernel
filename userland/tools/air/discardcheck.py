#!/usr/bin/env python3
"""air.discard_fragment: the shape here, and the value on the phone.

A discard is the one operation in this frontend whose MEANING was chosen rather
than measured. air_intrinsics.c translates it to NIR's `demote` and not to
NIR's `terminate`, with the reasons written there -- Resident Evil 4's
PS_DrawByTiles discards and then asks air.simd_is_helper_thread eight times,
and Apple's own declaration of it carries `nounwind` and not `noreturn`. Both
of those are about what Metal means. Neither is about what this hardware does
with the translation, and that is what this file is for.

What is decidable on this machine, and is checked below:

  - the module translates, and the NIR carries `demote` -- not `terminate`,
    which is the whole of the judgement, and not nothing, which is what a
    dropped intrinsic would leave;
  - nir_shader_info.fs.uses_discard is set. The frontend sets it by hand
    because nir_shader_gather_info does not run on this path, and it is not
    bookkeeping: nir_to_tgsi_info turns it into `uses_kill`, which is what a
    Gallium driver reads before it allows an early depth test. It is checked
    here THROUGH air_run, which refuses a discarding shader by name and can
    only do so if the flag arrived;
  - ir3 compiles it, and what the Adreno instruction is. It is `kill`, for
    both kinds: the a6xx encoder maps DEMOTE onto KILL
    (src/freedreno/isa/encode.c) and the ISA description has no demote bitset
    at all, so the distinction this frontend makes in NIR does not survive
    into the machine code on this GPU.

What is NOT decidable here, and is emitted for the phone instead:

  - whether a discarded fragment reaches the framebuffer. softpipe cannot say:
    nir_to_tgsi sends demote to TGSI_OPCODE_DEMOTE and tgsi_exec.c has no case
    for it, so the interpreter aborts on its own assert(0) rather than
    answering. That is why air_run refuses these rather than running them.
  - what a derivative computes in a lane whose quad partners were discarded.
    This is the question the choice of demote was made for, and no engine on
    this machine will answer it.

usage: discardcheck.py [-v]                  the shape, here
       discardcheck.py --emit DIR            modules + rc.jobs, for the phone
       discardcheck.py --check DIR RUN.LOG   judge what the phone printed
"""

import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))
IR3 = os.environ.get(
    "IR3", os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler"))
# the redwood's Adreno 7c+ Gen 3 (A635), as corpus.py names it
CHIP_ID = os.environ.get("AIR_CHIP_ID", "0x007506030500")

HEAD = """source_filename = "dc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare void @air.discard_fragment() local_unnamed_addr
declare <2 x float> @air.dfdx.v2f32(<2 x float>) local_unnamed_addr
"""

TAIL = """
!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @%(name)s, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{%(inputs)s}
%(inputmd)s
"""

# (1) The one unambiguous question: does a discarded fragment reach the
#     framebuffer at all? The shader writes four numbers nothing else in this
#     repository writes and then discards every fragment, so the attachment
#     must still hold what air_run cleared it to.
DISCARD_ALL = HEAD + """
define <{ <4 x float> }> @discard_all() local_unnamed_addr {
  tail call void @air.discard_fragment()
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> <float 7.0, float 8.0, float 9.0, float 10.0>, 0
  ret <{ <4 x float> }> %r
}
""" + TAIL % dict(name="discard_all", inputs="", inputmd="")

# (2) The question the choice of demote was made for. The fragment at (0, 0) is
#     kept and the three other lanes of its quad are discarded -- air_run draws
#     a 4x4 target and reads back the pixel at (0, 0), whose quad partners are
#     (1, 0), (0, 1) and (1, 1) -- and then the kept lane takes a derivative,
#     which can only be computed from its neighbours' values.
#
#     Metal's position is a window coordinate with the origin at the top left
#     and pixel centres at half-integers, so x > 1.0 or y > 1.0 is exactly
#     "not the pixel at (0, 0)". The derivative is of 3 * x, so the answer is
#     3 if the neighbour's value was there to be read.
#
#     THIS TEST IS INFORMATIVE IN ONE DIRECTION ONLY, and that is worth saying
#     before anybody reads a pass as an answer: under demote semantics a
#     compiler may hoist `3 * x` above the discard, in which case the
#     neighbour's value is in its register whatever the discard did, and 3
#     comes back either way. Anything OTHER than 3 is a finding -- the lane was
#     gone -- and 3 is consistent with both a working demote and a kill whose
#     victim had already done the arithmetic.
DISCARD_QUAD = HEAD + """
define <{ <4 x float> }> @discard_quad(<4 x float> %pos) local_unnamed_addr {
  %x = extractelement <4 x float> %pos, i32 0
  %y = extractelement <4 x float> %pos, i32 1
  %bx = fcmp ogt float %x, 1.0
  %by = fcmp ogt float %y, 1.0
  %out = or i1 %bx, %by
  br i1 %out, label %kill, label %keep

kill:
  tail call void @air.discard_fragment()
  br label %keep

keep:
  %v3 = fmul float %x, 3.0
  %v0 = insertelement <2 x float> undef, float %v3, i32 0
  %v1 = insertelement <2 x float> %v0, float %v3, i32 1
  %d = tail call <2 x float> @air.dfdx.v2f32(<2 x float> %v1)
  %dx = extractelement <2 x float> %d, i32 0
  %o0 = insertelement <4 x float> undef, float %dx, i32 0
  %o1 = insertelement <4 x float> %o0, float %dx, i32 1
  %o2 = insertelement <4 x float> %o1, float %dx, i32 2
  %o3 = insertelement <4 x float> %o2, float %dx, i32 3
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %o3, 0
  ret <{ <4 x float> }> %r
}
""" + TAIL % dict(
    name="discard_quad", inputs="!7",
    inputmd='!7 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", '
            '!"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}')

# What each module must print, and what a different answer would mean. The
# expectations live here and are not carried to the device, so a run cannot be
# made to agree by reading the answer.
CASES = [
    dict(id="discard_all", src=DISCARD_ALL, expect=[0.0, 0.0, 0.0, 0.0],
         means="the fragment was discarded: the attachment still holds the clear "
               "colour. 7 8 9 10 would mean the discard did nothing at all, and "
               "769 of Resident Evil 4's fragment modules would be drawing pixels "
               "they meant to throw away"),
    dict(id="discard_quad", src=DISCARD_QUAD, expect=[3.0, 3.0, 3.0, 3.0],
         means="the kept lane could still read its discarded neighbour, which is "
               "what Metal promises and what demote is for. Any other number means "
               "the neighbour was gone -- but see the comment above: 3 is also what "
               "a hoisted multiply would give, so only a failure here is decisive"),
]


def assemble(src, path):
    ll = path + ".ll"
    with open(ll, "w") as f:
        f.write(src)
    q = subprocess.run(["llvm-as", "-o", path, ll], capture_output=True)
    if q.returncode:
        return "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    return None


def numbers_after(text, marker="came back as"):
    i = text.find(marker)
    if i < 0:
        return None
    out = []
    for line in text[i:].splitlines()[1:]:
        t = line.strip()
        if t.startswith("air_run:") or t.startswith("=== "):
            break
        try:
            out.append(float(t))
        except ValueError:
            continue
    return out


def shape(verbose):
    """Everything about a discard that is decidable without a GPU."""
    d = tempfile.mkdtemp(prefix="discardcheck.")
    bad = 0
    for case in CASES:
        bc = os.path.join(d, case["id"] + ".bc")
        err = assemble(case["src"], bc)
        if err:
            print("REFUSED %-14s %s" % (case["id"], err))
            bad += 1
            continue

        # 1. it translates
        out = subprocess.run([AIR2NIR, "--report", bc], capture_output=True,
                             text=True).stdout
        if "translate=ok" not in out:
            print("WRONG   %-14s does not translate: %s" % (case["id"], out.strip()[:120]))
            bad += 1
            continue

        # 2. the NIR carries demote and not terminate. ir3_compiler prints the
        #    NIR it was given, which is the only NIR printer in this build.
        ir3 = subprocess.run([IR3, "--chip-id", CHIP_ID, "-v", bc, case["id"]],
                             capture_output=True, text=True)
        nir = ir3.stdout + ir3.stderr
        has_demote = re.search(r"@demote\b", nir) is not None
        has_terminate = re.search(r"@terminate\b", nir) is not None
        if not has_demote or has_terminate:
            print("WRONG   %-14s NIR has %s" % (
                case["id"], "terminate" if has_terminate else "no demote"))
            bad += 1
            continue

        # 3. ir3 compiled it, and what the instruction is
        if ir3.returncode != 0:
            print("WRONG   %-14s ir3 refused it: %s"
                  % (case["id"], (ir3.stderr or ir3.stdout).strip()[-120:]))
            bad += 1
            continue
        kills = len(re.findall(r"^\s*kill ", nir, re.M))
        demotes = len(re.findall(r"^\s*demote ", nir, re.M))

        # 4. air_run refuses it by name, which is also how uses_discard is
        #    checked: the refusal reads that flag and nothing else.
        run = subprocess.run([AIR_RUN, bc], capture_output=True, text=True)
        text = run.stdout + run.stderr
        refused = "discards fragments" in text
        if not refused:
            vals = numbers_after(text)
            print("WRONG   %-14s air_run did not refuse it (uses_discard not set?): %s"
                  % (case["id"], vals if vals is not None else text.strip()[-100:]))
            bad += 1
            continue

        print("shape   %-14s translates, NIR demote, ir3 ok (%d kill, %d demote in the "
              "Adreno code), air_run refuses it by name"
              % (case["id"], kills, demotes))
        if verbose:
            for line in nir.splitlines():
                if re.match(r"^\s*(kill|demote) ", line):
                    print("          %s" % line.strip())

    print()
    print("discardcheck: %d of %d shapes as expected" % (len(CASES) - bad, len(CASES)))
    print("discardcheck: NO VALUE WAS COMPARED HERE. softpipe's TGSI interpreter has no "
          "DEMOTE case at all (tgsi_exec.c), so what a discarded fragment leaves in the "
          "framebuffer cannot be measured on this machine; `--emit` writes the two "
          "modules and a job list for the phone, which can")
    print("discardcheck: and the Adreno instruction is `kill` for a demote as for a "
          "terminate -- src/freedreno/isa/encode.c maps OPC_DEMOTE to OPC_KILL and "
          "ir3-cat0.xml has no demote bitset -- so the distinction this frontend makes "
          "in NIR is not in the machine code on this GPU, and whether that costs "
          "anything is exactly what the phone is being asked")
    return 1 if bad else 0


JOBS = """# Commands run by the launchd job com.redwood.demo, generated by
# tools/air/discardcheck.py --emit. Do not edit: regenerate it.
#
# WHAT A DISCARDED FRAGMENT DOES ON THE ADRENO (RC_JOBS=rc.jobs.airdiscard).
# air.discard_fragment is translated as NIR's `demote` and not `terminate`
# (air_intrinsics.c says why, from the shaders and from Apple's own
# declaration). Nothing on the host can measure what that does: softpipe's
# TGSI interpreter has no DEMOTE case at all, so air_run refuses a discarding
# shader there rather than aborting inside it. These two modules are that
# measurement, and they need a GPU.
#
# Carried into %(there)s by build.sh EXTRA_SHARE. The directory's own name is
# in the paths at the bottom of this file, so emit it under that name or
# regenerate this file from the one you used:
#   python3 tools/air/discardcheck.py --emit <dir>
#   userland/build.sh ... EXTRA_SHARE=<dir> RC_JOBS=rc.jobs.airdiscard
# and judged afterwards, on the host, against expectations that are NOT in the
# image:
#   python3 tools/air/discardcheck.py --check <dir> <the boot log>
#
# Read the "air_run: <module> on ..." line of each block before its numbers: it
# names the screen, and a fallback to softpipe would refuse both of these
# instead of answering.
#
# The three greps that read the log, for a reader who has only the log:
#   grep -a 'air_run: .*discard_all' -A 8 LOG     # expect 0 0 0 0
#   grep -a 'air_run: .*discard_quad' -A 8 LOG    # expect 3 3 3 3
#   grep -ac 'discards fragments' LOG             # must be 0: that sentence is
#                                                 # softpipe refusing, so the
#                                                 # Adreno was never asked
# (-a on every one of them: a boot log has NUL bytes in it.)
#
%(expect)s
"""


def emit(outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = []
    for case in CASES:
        bc = os.path.join(outdir, case["id"] + ".bc")
        err = assemble(case["src"], bc)
        if err:
            print("discardcheck: %s: %s" % (case["id"], err), file=sys.stderr)
            return 1
        os.remove(bc + ".ll")
        manifest.append(dict(id=case["id"], expect=case["expect"], means=case["means"]))
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f)

    there = "/redwood/share/" + os.path.basename(os.path.normpath(outdir))
    lines = []
    for m in manifest:
        lines.append("# %s must print %s --\n#   %s"
                     % (m["id"], " ".join("%g" % v for v in m["expect"]),
                        "\n#   ".join(wrap(m["means"], 74))))
    body = "\n".join(lines) + "\n"
    with open(os.path.join(outdir, "rc.jobs"), "w") as f:
        f.write(JOBS % dict(there=there, expect=body))
        for m in manifest:
            f.write("/redwood/bin/air_run %s/%s.bc\n" % (there, m["id"]))
    print("discardcheck: %d modules in %s, with rc.jobs and manifest.json"
          % (len(manifest), outdir))
    print("discardcheck: the greps that read its log, whatever carried it:")
    print("  grep -a 'air_run: .*discard_all' -A 8 LOG     # expect 0 0 0 0")
    print("  grep -a 'air_run: .*discard_quad' -A 8 LOG    # expect 3 3 3 3")
    print("  grep -ac 'discards fragments' LOG             # must be 0: that is "
          "softpipe refusing, which means the Adreno was not there")
    return 0


def wrap(s, n):
    out, line = [], ""
    for w in s.split():
        if len(line) + len(w) + 1 > n:
            out.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        out.append(line)
    return out


def check(outdir, logpath):
    mani = os.path.join(outdir, "manifest.json")
    if os.path.getmtime(logpath) < os.path.getmtime(mani):
        print("discardcheck: %s is older than %s, so it cannot be this suite's log"
              % (os.path.basename(logpath), os.path.basename(mani)), file=sys.stderr)
        return 1
    with open(mani) as f:
        manifest = json.load(f)
    text = open(logpath, errors="replace").read()
    bad = missing = 0
    for m in manifest:
        # the block this module opened: air_run names the module it was given
        i = text.find(m["id"] + ".bc")
        if i < 0:
            print("missing %-14s no block in the log" % m["id"])
            missing += 1
            continue
        block = text[i:i + 4000]
        if "discards fragments" in block:
            print("refused %-14s air_run refused it, so this ran on softpipe and not "
                  "on the Adreno" % m["id"])
            bad += 1
            continue
        vals = numbers_after(block)
        if not vals or len(vals) < 4:
            print("short   %-14s printed %s" % (m["id"], vals))
            bad += 1
            continue
        if vals[:4] != [float(v) for v in m["expect"]]:
            print("WRONG   %-14s got %s want %s -- %s"
                  % (m["id"], vals[:4], m["expect"], m["means"]))
            bad += 1
        else:
            print("ok      %-14s %s -- %s" % (m["id"],
                                              " ".join("%g" % v for v in vals[:4]),
                                              m["means"]))
    print()
    print("discardcheck: %d of %d agreed, %d differed, %d not in the log"
          % (len(manifest) - bad - missing, len(manifest), bad, missing))
    return 1 if (bad or missing) else 0


def main(argv):
    verbose = "-v" in argv
    if "--emit" in argv:
        return emit(argv[argv.index("--emit") + 1])
    if "--check" in argv:
        i = argv.index("--check")
        return check(argv[i + 1], argv[i + 2])
    for p, what in ((AIR2NIR, "air2nir"), (AIR_RUN, "air_run"), (IR3, "ir3_compiler")):
        if not os.path.exists(p):
            print("discardcheck: no %s at %s (run tools/air/mk-host.sh)" % (what, p),
                  file=sys.stderr)
            return 1
    return shape(verbose)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
