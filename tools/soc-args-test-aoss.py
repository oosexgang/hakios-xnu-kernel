#!/usr/bin/env python3
"""The generator must find the AOSS mailbox however a phone's device tree describes it.

The same SM7325 phone gave soc_aoss from the repository's reference tree and none from the tree the
running phone itself exports (/sys/firmware/fdt): the reference calls the node
qcom,sm8350-aoss-qmp, the phone's Android kernel calls it qcom,qmp-mbox on /soc/qcom,qmp-aop. This test
builds small device trees of its own (no part of a real phone's tree is committed) and checks
tools/soc-args.py on each:

  mainline form                 -> soc_aoss found
  vendor-kernel form            -> soc_aoss found
  both, and a reg-less decoy    -> the one with a reg, and only one line
  neither                       -> no soc_aoss, and no failure
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
SOC_ARGS = os.path.join(HERE, "soc-args.py")


from fdtbuild import cells, compat, fdt  # noqa: E402  (tools/fdtbuild.py)


def soc_with(**nodes):
    return {"": ({}, {"soc": ({"#address-cells": cells(1), "#size-cells": cells(1)}, nodes)})}


def run(tree):
    with tempfile.NamedTemporaryFile(suffix=".dtb", delete=False) as f:
        f.write(fdt(tree))
        path = f.name
    try:
        # an IPCC node is there in every tree, so that the generator has something to print
        return subprocess.run([sys.executable, SOC_ARGS, path], capture_output=True, text=True)
    finally:
        os.unlink(path)


IPCC = ("qcom,ipcc@408000", ({"compatible": compat("qcom,ipcc"), "reg": cells(0x408000, 0x1000)}, {}))
failures = []


def check(label, tree, want):
    got = run(tree)
    lines = [l for l in got.stdout.splitlines() if l.startswith("soc_aoss=")]
    ok = got.returncode == 0 and lines == want
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        print("     want %r, got %r (exit %d) %s" % (want, lines, got.returncode, got.stderr.strip()[:120]))


mainline = ("power-controller@c300000", ({"compatible": compat("qcom,sm8350-aoss-qmp"), "reg": cells(0xC300000, 0x400)}, {}))
vendor = ("qcom,qmp-aop@c300000", ({"compatible": compat("qcom,qmp-mbox"), "reg": cells(0xC300000, 0x400)}, {}))
decoy = ("qcom,qmp-aop@dead000", ({"compatible": compat("qcom,qmp-mbox")}, {}))     # no reg: must be ignored
other_soc = ("power-controller@1000000", ({"compatible": compat("qcom,sc7280-aoss-qmp"), "reg": cells(0x1000000, 0x400)}, {}))

check("mainline form", soc_with(**dict([IPCC, mainline])), ["soc_aoss=0xc300000"])
check("vendor-kernel form", soc_with(**dict([IPCC, vendor])), ["soc_aoss=0xc300000"])
check("a reg-less decoy beside the mainline node", soc_with(**dict([IPCC, decoy, mainline])), ["soc_aoss=0xc300000"])
check("a reg-less decoy only", soc_with(**dict([IPCC, decoy])), [])
check("another SoC's compatible (qcom,sc7280-aoss-qmp)", soc_with(**dict([IPCC, other_soc])), ["soc_aoss=0x1000000"])
check("neither: no soc_aoss, no failure", soc_with(**dict([IPCC])), [])

if failures:
    sys.exit("soc-args-test-aoss: %d FAILED" % len(failures))
print("soc-args-test-aoss: all passed")
