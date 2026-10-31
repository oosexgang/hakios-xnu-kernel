#!/usr/bin/env python3
"""The two interface keys about rendering in place, neither of which has an oracle.

Metal has two ways for a fragment shader to work on the attachment it is drawing
to, and this frontend can check neither by value on this machine:

  - `[[color(n)]]`, an `air.render_target` INPUT: framebuffer fetch, the current
    value of render target n.
  - `[[raster_order_group(n)]]`, `air.raster_order_group` on a resource: accesses
    to it from overlapping fragments happen in rasterization order.

An `air.render_target` *input* is the one interface key whose result no harness
in this directory can compare against a number. `cpuref.py` runs the module
under LLVM on the processor, where there is no framebuffer; `air_run` draws it,
but on softpipe, whose TGSI interpreter has no FBFETCH at all
(`tgsi_exec.c` does not implement the opcode, so the only Gallium driver here
that could answer is the one on the phone). So this file checks the two things
that *are* decidable on this machine, and says plainly that it checks no value:

  1. the frontend emits NIR's own form of a framebuffer fetch -- `load_output`
     with `io_semantics.fb_fetch_output` -- at the render target the module
     names, and sets `info.fs.uses_fbfetch_output`, without which a draw on
     a6xx gets no flush between overlapping primitives and the read returns
     whatever the tile buffer happened to hold (`fd6_emit.cc`, build_prim_mode);
  2. ir3 compiles that to the Adreno instruction that reads the framebuffer --
     `isamm` with `s2en`, the sample-from-register form `nir_lower_fb_read`'s
     `txf_ms_fb` becomes.

and three cases that are about the frontend's own judgement rather than the
hardware: a target index that reaches the intrinsic, an integer target that
must be REFUSED rather than silently converted, and an argument marked
`air.arg_unused`, which must not turn the shader into a framebuffer-fetch
shader at all.

The modules are generated here rather than taken from the corpus, for the
reason the rest of this directory generates them: a corpus module mixes the
case with everything else it does, and `path_blit_frag` reads target 2 while
writing three, so a shader that read target 0 by mistake would still pass over
it. The shapes are the three the corpus declares (`float4`, `half4` and a
scalar `float` at a non-zero target) and one it does not.

The second one used to be dropped in SILENCE -- the branch that matches
`air.texture` returns before the loop that refuses an unhandled `air.*` key, so
every other key in that node was invisible -- and a shader that asks for
ordering and gets none, with nothing in any log saying so, is the shape of bug
this project distrusts most. a6xx cannot give what it asks: the ask is per
resource and `GRAS_SC_CNTL.SINGLE_PRIM_MODE` is per draw. So the shader is
translated, the draw is ordered as a whole -- more than was asked, which is safe
and slower -- and the difference is recorded as a NOTE that `air2nir --report`
prints. The cases below check that the note is there and says what the runtime
has to do, because a caveat nobody prints is the same as no caveat.

usage: fbfetchcheck.py [-v]
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
IR3 = os.environ.get(
    "IR3", os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler"))
CHIP_ID = os.environ.get("AIR_CHIP_ID", "0x007506030500")

SKELETON = """source_filename = "fbf"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define %(retty)s @fbf(%(argty)s %%0) local_unnamed_addr {
%(body)s
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.compile_options = !{!10}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @fbf, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.render_target", i32 %(rt)d, !"air.arg_type_name", !"%(inname)s", !"air.arg_name", !"d0"%(unused)s}
!10 = !{!"air.compile.framebuffer_fetch_enable"}
"""


def module(inname, argty, rt, retty, outname, body, unused=False):
    return SKELETON % dict(inname=inname, argty=argty, rt=rt, retty=retty,
                           outname=outname, body=body,
                           unused=', !"air.arg_unused"' if unused else "")


ROG = """source_filename = "rog"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%%struct._texture_2d_t = type opaque

define <4 x float> @rog(ptr addrspace(1) %%t) local_unnamed_addr {
  ret <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0>
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @rog, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.write", !"air.raster_order_group", i32 %(group)d, !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"tex"}
"""


def rog(group):
    return ROG % dict(group=group)


# Each case: (label, source, what must be true of it).
#
# The body returns the fetched value so that it is not dead code: a
# `load_output` carries CAN_ELIMINATE, so a case that dropped the value on the
# floor would pass by having nothing left to get wrong.
PASS_THROUGH = "  ret %s %%0"
CASES = [
    ("float4 at target 0",
     module("float4", "<4 x float>", 0, "<4 x float>", "float4",
            PASS_THROUGH % "<4 x float>"),
     dict(slot="FRAG_RESULT_DATA0", fetch=True, isamm=True)),

    # half: the fetch itself is four 32-bit floats whatever the argument says
    # (nir_lower_fb_read fixes its txf_ms_fb at 4 x f32), so this case is the
    # one that would catch a narrowing done with the wrong conversion -- `fit`
    # would have used u2f and turned the bits into a number.
    ("half4 at target 0",
     module("half4", "<4 x half>", 0, "<4 x half>", "half4",
            PASS_THROUGH % "<4 x half>"),
     dict(slot="FRAG_RESULT_DATA0", fetch=True, isamm=True, f2f16=True)),

    # path_blit_frag's shape: a scalar coverage value read from target 2 while
    # the shader writes target 0. The target number has to reach the
    # intrinsic, which is the whole of this case.
    ("float at target 2, written to target 0",
     module("float", "float", 2, "<4 x float>", "float4",
            "  %v = insertelement <4 x float> undef, float %0, i32 0\n"
            "  %w = shufflevector <4 x float> %v, <4 x float> undef, "
            "<4 x i32> zeroinitializer\n"
            "  ret <4 x float> %w"),
     dict(slot="FRAG_RESULT_DATA2", fetch=True, isamm=True)),

    # An integer target. The lowering hands back floats converted from the
    # attachment's format, so this would arrive as the wrong VALUE and nothing
    # would say so: it must be refused instead.
    ("int4 at target 0 (must be refused)",
     module("int4", "<4 x i32>", 0, "<4 x i32>", "int4",
            PASS_THROUGH % "<4 x i32>"),
     dict(refuse="air.render_target input of type int4")),

    # Marked unused: must translate, and must NOT make this a framebuffer-fetch
    # shader. uses_fbfetch_output alone costs the draw a flush per overlapping
    # primitive on a6xx, for a value nothing reads.
    ("float4 at target 0, air.arg_unused (must not fetch)",
     module("float4", "<4 x float>", 0, "<4 x float>", "float4",
            PASS_THROUGH % "<4 x float>", unused=True),
     dict(fetch=False)),

    # `[[raster_order_group(0)]]` on a written texture, which is what
    # `inplace_copy_{lpf,lph}` carries. It must translate -- the coarser
    # ordering is still correct -- and it must SAY SO: the note has to name
    # what the runtime does instead, or the caveat is as good as dropped.
    ("raster order group 0 on an image (must note)",
     rog(0),
     dict(note="blend_coherent")),

    # A group number the bitmask cannot hold. Recording it would fold it onto a
    # different group, which is worse than not recording it, so it is refused.
    ("raster order group 99 (must be refused)",
     rog(99),
     dict(refuse="raster order group 99")),
]


def build(ll, work, n):
    bc = os.path.join(work, "fbf%d.bc" % n)
    src = os.path.join(work, "fbf%d.ll" % n)
    with open(src, "w") as f:
        f.write(ll)
    q = subprocess.run(["llvm-as", "-o", bc, src], capture_output=True)
    if q.returncode != 0:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    return bc, None


def check(ll, want, work, n, verbose):
    """Returns (ok, message)."""
    bc, err = build(ll, work, n)
    if err:
        return False, err

    q = subprocess.run([AIR2NIR, bc], capture_output=True)
    nir = q.stdout.decode()
    why = q.stderr.decode().strip()
    if verbose:
        print(nir or why)

    if "note" in want:
        r = subprocess.run([AIR2NIR, "--report", bc], capture_output=True)
        line = r.stdout.decode().strip()
        if verbose:
            print(line)
        if "translate=ok" not in line:
            return False, "did not translate: " + (why or line)
        if "\tnotes=" not in line:
            return False, "translated with no note at all, so the caveat is dropped"
        note = line.split("\tnotes=", 1)[1].split("\t")[0]
        if want["note"] not in note:
            return False, "noted, but without %r: %s" % (want["note"], note)
        return True, "noted: " + note

    if "refuse" in want:
        if q.returncode == 0:
            return False, "translated, and it should have been refused"
        if want["refuse"] not in why:
            return False, "refused, but for %r" % why
        return True, "refused: " + why.split(".bc: ", 1)[-1]

    if q.returncode != 0:
        return False, "air2nir: " + (why or "no output")

    fetch = "fbfetch" in nir
    if fetch != want["fetch"]:
        return False, ("the NIR carries a framebuffer fetch and should not"
                       if fetch else "the NIR carries no framebuffer fetch")
    if not want["fetch"]:
        if "uses_fbfetch_output" in nir:
            return False, "info.fs.uses_fbfetch_output is set for an unused argument"
        return True, "no fetch, as an unused argument should have none"

    if "uses_fbfetch_output: true" not in nir:
        return False, ("the intrinsic is there but info.fs.uses_fbfetch_output is not "
                       "set, so a6xx would not flush between overlapping primitives")
    expect = "io location=%s slots=1 fbfetch coherent" % want["slot"]
    if expect not in nir:
        return False, "no %r in the NIR" % expect
    if want.get("f2f16") and "f2f16" not in nir:
        return False, "the half target is not narrowed with f2f16"

    if want.get("isamm"):
        q = subprocess.run([IR3, "-c", CHIP_ID, bc], capture_output=True)
        asm = q.stdout.decode()
        if verbose:
            print(asm)
        if q.returncode != 0:
            return False, "ir3: " + q.stderr.decode().strip().splitlines()[-1]
        fb = [l.strip() for l in asm.splitlines()
              if l.strip().startswith("isamm") and "s2en" in l]
        if not fb:
            return False, "ir3 compiled it, but emitted no isamm/s2en framebuffer read"
        return True, fb[0]

    return True, "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the NIR and the Adreno instructions of each case")
    args = ap.parse_args()

    for prog in (AIR2NIR, IR3):
        if not os.path.exists(prog):
            print("fbfetchcheck: run tools/air/mk-host.sh first (%s)" % prog,
                  file=sys.stderr)
            return 2

    ok = 0
    with tempfile.TemporaryDirectory(prefix="fbfetchcheck") as work:
        for n, (label, ll, want) in enumerate(CASES):
            good, msg = check(ll, want, work, n, args.verbose)
            ok += good
            print("%-4s %-46s %s" % ("" if good else "FAIL", label, msg))

    print("fbfetchcheck: %d of %d cases as expected" % (ok, len(CASES)))
    print("fbfetchcheck: no VALUE is compared here and none can be on this machine -- "
          "softpipe has no FBFETCH, so what the framebuffer read returns is a question "
          "only the phone can answer")
    return 0 if ok == len(CASES) else 1


if __name__ == "__main__":
    sys.exit(main())
