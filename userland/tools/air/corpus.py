#!/usr/bin/env python3
"""The corpus report: how far every module of every .metallib gets.

This is the project's progress meter (docs/air.md section 5). Each module goes
through stages, and each module is counted under the first one it fails:

  container   found in the .metallib and unwrapped
  reader      our bitcode reader read it (and agrees with llvm-dis: that is
              checked separately, by tools/air/reader-check.sh)
  translate   air_to_nir produced a shader and nir_validate passed
  ir3         ir3 compiled it for the Adreno without error

Nothing here says a shader is correct: that needs the CPU reference and a run
on the GPU. A module that reaches ir3 has compiled, which is not the same
thing, and the report never claims otherwise.

usage: corpus.py [--ir3] [--json FILE] [--md FILE] FILE.metallib...
       corpus.py [--ir3] ... --list FILE      (one library path per line)
"""
import argparse
import collections
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
IR3 = os.environ.get(
    "IR3", os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler"))
# the redwood's Adreno 7c+ Gen 3 (A635), as freedreno names it
CHIP_ID = os.environ.get("AIR_CHIP_ID", "0x007506030500")


# The mains of the other tools: each one is linked into its own binary and into
# nothing else, so a change to it cannot make a different tool stale. Treating
# every file of tools/mesa/air as a dependency of every binary refused a run
# whose numbers were perfectly current -- which costs more than the guard
# saves, because the next refusal gets worked around rather than read.
TOOL_MAINS = {"air2nir.c": "air2nir", "air_run.c": "air_run",
              # the library the Metal driver links and its test (air_metal.h):
              # in no binary whose numbers this script reports, so a change to
              # them makes none of those stale.
              "air_metal.c": "libredwoodair", "air_metal.h": "libredwoodair",
              "air_metal_test.c": "air_metal_test"}


def refuse_if_stale(prog, what):
    """A tool older than the frontend it contains is measuring the past.

    ir3_compiler is not in the host build's default target, so a plain
    `ninja -C build/mesa-host/build` leaves it as it was while air2nir beside
    it is rebuilt. That produced a column of numbers from the old frontend and
    a translate column from the new one: on 20 October the two read 315 and
    313, and the missing two looked like modules ir3 could not compile. They
    compiled perfectly; the compiler was six hours old. Both columns came from
    the same table and only one of them was current, which is the worst case --
    a stale tool whose output is plausible beside a fresh one.

    So the age is checked rather than assumed, against every source of the
    frontend, and the run is refused instead of warned: a number nobody can
    trust is worse than no number.
    """
    if not os.path.exists(prog):
        return
    src = os.path.join(ROOT, "tools/mesa/air")
    newest, name = 0.0, None
    for f in os.listdir(src):
        if not f.endswith((".c", ".h")):
            continue
        if TOOL_MAINS.get(f) not in (None, os.path.basename(prog)):
            continue      # another tool's main: not linked into this binary
        t = os.path.getmtime(os.path.join(src, f))
        if t > newest:
            newest, name = t, f
    if newest and os.path.getmtime(prog) < newest:
        print("corpus.py: %s is older than tools/mesa/air/%s, so its column would be "
              "the previous frontend's. Build it: ninja -C build/mesa-host/build %s"
              % (what, name, os.path.relpath(prog, os.path.join(ROOT, "build/mesa-host/build"))),
              file=sys.stderr)
        sys.exit(1)


def read_report(paths):
    """air2nir --report, which forks per module so one crash is one line."""
    rows = []
    for path in paths:
        out = subprocess.run([AIR2NIR, "--report", path], capture_output=True, text=True)
        for line in out.stdout.splitlines():
            if not line.startswith("module="):
                continue
            row = dict(kv.split("=", 1) for kv in line.split("\t") if "=" in kv)
            # two of the five libraries are both called default.metallib, so
            # the label carries the bundle they are in
            row["library"] = os.path.join(os.path.basename(os.path.dirname(path)),
                                          os.path.basename(path))
            row["path"] = path
            row["calls"] = [c for c in row.get("calls", "").split(",") if c]
            rows.append(row)
    return rows


def run_ir3(rows):
    """ir3 on every module that translated, one process each."""
    for row in rows:
        if row.get("translate") != "ok":
            continue
        out = subprocess.run([IR3, "--chip-id", CHIP_ID, row["path"], row["module"]],
                             capture_output=True, text=True, timeout=300)
        if out.returncode == 0:
            row["ir3"] = "ok"
            for line in out.stdout.splitlines():
                # "; FRAG prog 0/0: 5 instr, 0 nops, ..."
                if " prog " in line and " instr," in line:
                    row["ir3_instr"] = int(line.split(":")[1].split()[0])
                    break
        else:
            row["ir3"] = "fail"
            err = (out.stderr or out.stdout).strip().splitlines()
            row["ir3_error"] = err[-1][:200] if err else "no message"


def reason(row):
    if row.get("crash"):
        return "crashed: " + row["crash"]
    if row.get("reader") != "ok":
        return row.get("error", "reader failed")
    if row.get("translate") != "ok":
        return row.get("error", "translation failed")
    if row.get("ir3") == "fail":
        return "ir3: " + row.get("ir3_error", "")
    return None


def short_reason(r):
    """Group reasons so the table ranks causes, not instances."""
    if r is None:
        return None
    r = r.split(" (block ")[0]
    if r.startswith("intrinsic: air."):
        name = r.split("intrinsic: ")[1].split("(")[0].strip()
        # air.sample_texture_2d.v4f16 -> air.sample_texture_2d
        base = name.split(".")
        if len(base) > 2:
            name = ".".join(base[:2]) if base[1] in ("atomic", "wg") else ".".join(base[:2])
        return "intrinsic: " + name
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="*")
    # The system's corpus is five files and fits on a command line. A game's
    # does not: RE Engine compiles every shader variant into a .metallib of its
    # own and one pak holds 7 879 of them, which is past what argv takes, so
    # the list can come from a file (one path per line) instead.
    ap.add_argument("--list", help="file of library paths, one per line")
    ap.add_argument("--ir3", action="store_true", help="also compile through ir3")
    ap.add_argument("--json")
    ap.add_argument("--md")
    args = ap.parse_args()

    libs = list(args.libs)
    if args.list:
        with open(args.list) as f:
            libs += [ln.strip() for ln in f if ln.strip()]
    if not libs:
        ap.error("no libraries: give paths, or --list FILE")

    rows = read_report(libs)
    if not rows:
        sys.exit("corpus.py: no modules; is %s built?" % AIR2NIR)
    if args.ir3:
        refuse_if_stale(IR3, "ir3_compiler")
        run_ir3(rows)

    total = len(rows)
    stages = collections.OrderedDict()
    stages["container"] = total
    stages["reader"] = sum(1 for r in rows if r.get("reader") == "ok")
    stages["translate"] = sum(1 for r in rows if r.get("translate") == "ok")
    if args.ir3:
        stages["ir3"] = sum(1 for r in rows if r.get("ir3") == "ok")

    by_stage = collections.Counter(r.get("stage", "?") for r in rows)
    ok_by_stage = collections.Counter(r.get("stage", "?") for r in rows
                                      if r.get("translate") == "ok")
    ir3_by_stage = collections.Counter(r.get("stage", "?") for r in rows
                                       if r.get("ir3") == "ok")
    reasons = collections.Counter(short_reason(reason(r)) for r in rows
                                  if reason(r) is not None)
    intrinsics = collections.Counter(c for r in rows for c in r["calls"])
    no_intrinsics = [r for r in rows if not r["calls"]]

    out = []
    out.append("# AIR corpus report\n")
    out.append("What every module of the system's Metal libraries does when put through the\n"
               "frontend. Generated by `tools/air/corpus.py`; see docs/air.md section 5 for\n"
               "what the stages mean. **Reaching ir3 means it compiled, not that it is\n"
               "correct**: correctness needs the CPU reference and a run on the GPU.\n")
    out.append("## Stages\n")
    out.append("| stage | modules | of %d |" % total)
    out.append("| --- | --- | --- |")
    for name, n in stages.items():
        out.append("| %s | %d | %.0f%% |" % (name, n, 100.0 * n / total))
    out.append("")

    out.append("## By stage of the shader\n")
    out.append("| what the module is | modules | translate | ir3 |")
    out.append("| --- | --- | --- | --- |")
    for st in sorted(by_stage):
        out.append("| %s | %d | %d | %s |" % (st, by_stage[st], ok_by_stage[st],
                                              ir3_by_stage[st] if args.ir3 else "-"))
    out.append("")

    # The container's TYPE tag and the module's own metadata are two
    # independent statements of what a function is; if they ever disagree, one
    # of the two readings is wrong and the report says so rather than picking.
    same = {"vertex": "vertex", "fragment": "fragment", "kernel": "kernel",
            "visible": "visible"}
    mismatch = [r for r in rows
                if r.get("container") in same and r.get("stage")
                and same[r["container"]] != r.get("stage")]
    out.append("The container's TYPE tag agrees with the module's own metadata on %d of %d; "
               "%d disagree.\n" % (total - len(mismatch), total, len(mismatch)))
    for r in mismatch[:20]:
        out.append("- `%s`: container says %s, metadata says %s" %
                   (r["module"], r["container"], r["stage"]))
    if mismatch:
        out.append("")

    libnames = sorted({r["library"] for r in rows})
    out.append("## By library\n")
    # A row per library is the report for a handful of system libraries and
    # noise for thousands of single-function ones; past that the count is the
    # only thing the table would say that the stage table does not.
    if len(libnames) <= 24:
        out.append("| library | modules | translate | ir3 |")
        out.append("| --- | --- | --- | --- |")
        for lib in libnames:
            rs = [r for r in rows if r["library"] == lib]
            out.append("| %s | %d | %d | %s |" % (
                lib, len(rs), sum(1 for r in rs if r.get("translate") == "ok"),
                sum(1 for r in rs if r.get("ir3") == "ok") if args.ir3 else "-"))
    else:
        out.append("%d libraries, %.2f modules each on average; not listed one by one.\n"
                   % (len(libnames), float(total) / len(libnames)))
    out.append("")

    out.append("## Modules that call no `air.*` at all\n")
    out.append("%d of %d (%.0f%%), of which %d translate and %s compile through ir3.\n" % (
        len(no_intrinsics), total, 100.0 * len(no_intrinsics) / total,
        sum(1 for r in no_intrinsics if r.get("translate") == "ok"),
        sum(1 for r in no_intrinsics if r.get("ir3") == "ok") if args.ir3 else "-"))

    if reasons:
        out.append("## What is not handled yet, by how many modules it stops\n")
        out.append("| modules | where it stops |")
        out.append("| --- | --- |")
        for why, n in reasons.most_common():
            out.append("| %d | %s |" % (n, why.replace("|", "\\|")))
        out.append("")

    # Translated, and not exactly as asked. A refusal is counted above and a
    # clean translation is counted in the stage table; this is the third thing,
    # which used to have nowhere to be counted and therefore went unsaid.
    notes = collections.Counter()
    for r in rows:
        for note in (r.get("notes") or "").split(";"):
            if note:
                notes[note] += 1
    if notes:
        out.append("## What was translated with a caveat\n")
        out.append("A module here was accepted and something other than exactly what it "
                   "asked for was done. Not a refusal and not a warning about this "
                   "frontend's own code: a thing the hardware cannot do exactly, where "
                   "doing something else is still correct.\n")
        out.append("| modules | what was asked, and what was done |")
        out.append("| --- | --- |")
        for note, n in notes.most_common():
            out.append("| %d | %s |" % (n, note.replace("|", "\\|")))
        out.append("")

    if intrinsics:
        out.append("## The `air.*` functions the corpus calls\n")
        out.append("%d distinct names in %d modules.\n" % (
            len(intrinsics), sum(1 for r in rows if r["calls"])))
        out.append("| calls | name |")
        out.append("| --- | --- |")
        for name, n in intrinsics.most_common():
            out.append("| %d | `%s` |" % (n, name))
        out.append("")

    text = "\n".join(out)
    if args.md:
        with open(args.md, "w") as f:
            f.write(text)
    else:
        print(text)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"total": total, "stages": stages, "rows": rows}, f, indent=1)

    print("corpus.py: %s" % ", ".join("%s %d/%d" % (k, v, total) for k, v in stages.items()),
          file=sys.stderr)


if __name__ == "__main__":
    main()
