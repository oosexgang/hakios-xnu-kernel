#!/usr/bin/env python3
"""tools/new-device.py: what it makes and what it refuses.

  the reference phone's own tree   -> "the SAME values", soc.args identical to the reference's
  another SoC's tree               -> says which values differ and which are missing; the README says so too
  not a device tree                -> a clear error, nothing made
  a bad name, an existing directory -> refused (an existing one is written into only with --force)
  --adb, the tree readable by root only (a stand-in adb; not on Windows, where a script is not a program)
                                   -> pulled through su, and the staged copy is removed again
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from fdtbuild import cells, compat, fdt  # noqa: E402

TOOL = os.path.join(HERE, "new-device.py")
REFERENCE_DTB = os.path.join(ROOT, "devices", "redwood", "prebuilt", "device-tree.dtb")
REFERENCE_ARGS = os.path.join(ROOT, "devices", "redwood", "soc.args")
failures = []


def run(*args, env=None):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, env=env)


def check(label, condition, detail=""):
    print("%s %s" % ("ok  " if condition else "FAIL", label))
    if not condition:
        failures.append(label)
        if detail:
            print("     " + detail.strip().replace("\n", "\n     ")[:600])


work = tempfile.mkdtemp(prefix="new-device-test-")
try:
    # 1. the reference phone's own tree
    out = os.path.join(work, "ref")
    r = run("same-soc", "--fdt", REFERENCE_DTB, "--out", out)
    check("the reference tree: exit 0", r.returncode == 0, r.stdout + r.stderr)
    check("the reference tree: says the SAME values", "the SAME values as the reference" in r.stdout, r.stdout)
    check("the reference tree: soc.args is byte for byte the reference's",
          os.path.exists(os.path.join(out, "soc.args")) and
          open(os.path.join(out, "soc.args"), "rb").read() == open(REFERENCE_ARGS, "rb").read().replace(b"\r\n", b"\n"))
    check("the reference tree: no carriage return in soc.args", b"\r" not in open(os.path.join(out, "soc.args"), "rb").read())
    check("the reference tree: README, pull-soc.sh and soc.args made",
          sorted(os.listdir(out)) == ["README.md", "pull-soc.sh", "soc.args"], str(sorted(os.listdir(out))))
    readme = open(os.path.join(out, "README.md"), encoding="utf-8").read()
    check("the reference tree: README names the board and carries the checklist",
          "Yupik" in readme and "- [ ] **Cellular**" in readme and "the same as the reference" in readme)

    # 2. another SoC: an IPCC at the usual place, the AOSS mailbox elsewhere, nothing else
    other = fdt({"": ({"model": b"Some Phone\0", "compatible": compat("vendor,some-phone", "qcom,sm9999")},
                      {"soc": ({"#address-cells": cells(1), "#size-cells": cells(1)}, {"qcom,ipcc@408000": ({"compatible": compat("qcom,ipcc"), "reg": cells(0x408000, 0x1000)}, {}),
                                    "power-controller@1000000": ({"compatible": compat("qcom,sc7280-aoss-qmp"),
                                                                  "reg": cells(0x1000000, 0x400)}, {})})})})
    other_path = os.path.join(work, "other.dtb")
    open(other_path, "wb").write(other)
    out = os.path.join(work, "other")
    r = run("some-phone", "--fdt", other_path, "--out", out)
    check("another SoC: exit 0", r.returncode == 0, r.stdout + r.stderr)
    check("another SoC: does not claim to be the reference", "SAME" not in r.stdout and "differs" in r.stdout, r.stdout)
    readme = open(os.path.join(out, "README.md"), encoding="utf-8").read()
    check("another SoC: README says which differs and which are missing",
          "**differs from the reference**" in readme and "**missing**" in readme and "Some Phone" in readme)
    check("another SoC: soc.args holds what was found",
          open(os.path.join(out, "soc.args")).read().split() == ["soc_ipcc=0x408000", "soc_aoss=0x1000000"],
          open(os.path.join(out, "soc.args")).read())

    # 3. refusals
    junk = os.path.join(work, "junk.dtb")
    open(junk, "wb").write(b"not a device tree at all")
    r = run("x", "--fdt", junk, "--out", os.path.join(work, "junk"))
    check("not a device tree: refused with a reason", r.returncode != 0 and "not a flattened device tree" in (r.stdout + r.stderr), r.stdout + r.stderr)
    check("not a device tree: nothing made", not os.path.exists(os.path.join(work, "junk")))
    r = run("Bad Name!", "--fdt", REFERENCE_DTB, "--out", os.path.join(work, "bad"))
    check("a bad name: refused", r.returncode != 0 and "lower case" in (r.stdout + r.stderr), r.stdout + r.stderr)
    r = run("again", "--fdt", REFERENCE_DTB, "--out", os.path.join(work, "ref"))
    check("an existing directory: refused", r.returncode != 0 and "exists" in (r.stdout + r.stderr), r.stdout + r.stderr)
    r = run("again", "--fdt", REFERENCE_DTB, "--out", os.path.join(work, "ref"), "--force")
    check("an existing directory with --force: written", r.returncode == 0, r.stdout + r.stderr)
    r = run("x", "--fdt", os.path.join(work, "missing.dtb"), "--out", os.path.join(work, "m"))
    check("no such file: refused", r.returncode != 0 and "no such file" in (r.stdout + r.stderr), r.stdout + r.stderr)

    # 4. --adb with the tree readable by root only (a stand-in adb: a shell script, so not on Windows)
    if os.name != "nt":
        fake = os.path.join(work, "bin")
        os.makedirs(fake)
        state = os.path.join(work, "state")
        script = r"""#!/bin/sh
# a phone whose /sys/firmware/fdt only root can read: a plain pull fails; su copies it to a staging file
case "$1" in
pull)
	case "$2" in
	/sys/firmware/fdt) echo "adb: error: failed to stat remote object '/sys/firmware/fdt': Permission denied" >&2; exit 1 ;;
	/data/local/tmp/hakios-fdt) [ -f "$STATE" ] || exit 1; cp "$REF" "$3"; exit 0 ;;
	esac ;;
shell)
	case "$2" in
	*"cat /sys/firmware/fdt >"*) : > "$STATE"; exit 0 ;;
	*"rm -f /data/local/tmp/hakios-fdt"*) rm -f "$STATE"; echo removed >> "$STATE.log"; exit 0 ;;
	esac ;;
esac
exit 1
"""
        adb = os.path.join(fake, "adb")
        open(adb, "w", newline="\n").write(script)
        os.chmod(adb, os.stat(adb).st_mode | stat.S_IEXEC)
        env = dict(os.environ, PATH=fake + os.pathsep + os.environ["PATH"], STATE=state, REF=REFERENCE_DTB)
        r = run("from-adb", "--adb", "--out", os.path.join(work, "adb"), env=env)
        check("--adb, root-only tree: pulled through su and recognised", r.returncode == 0 and "the SAME values" in r.stdout, r.stdout + r.stderr)
        check("--adb, root-only tree: README says how it was got",
              "through su" in open(os.path.join(work, "adb", "README.md"), encoding="utf-8").read())
        check("--adb, root-only tree: the staged copy on the phone was removed again",
              os.path.exists(state + ".log") and not os.path.exists(state))
    else:
        print("skip --adb with a stand-in adb (Windows)")
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("new-device-test: %d FAILED" % len(failures))
print("new-device-test: all passed")
