#!/usr/bin/env python3
"""tools/hakios-device.py and the layers of tools/hakios-config.py: a common repository, a SoC's, a device's with a driver of its own.

Everything happens in a copy made for the test (the tools, configs/, the drivers' conf files, the SoC and device repositories):
  - a device repository's defconfig is found as if it were in configs/; the same name in two repositories is refused
  - its driver (devices/<name>/drivers/<sub>/conf) is a CONFIG_HAKIOS_ symbol like the common ones, goes into the kernel tree's
    conf/files `optional`, and is listed by external-files for apply.sh, sync-drivers.sh and export.sh
  - a device repository cannot declare `standard` files; a driver naming a file that is not there is a problem of `check`
  - `new` makes a device that `check` passes, with the phone drivers left out but the SoC's
  - the real tree's devices and SoCs hold together
"""
import os
import shutil
import subprocess
import sys
import tempfile

# (--repo commits: a machine with no git identity of its own is no reason for the test to fail)
for key, value in (("GIT_AUTHOR_NAME", "test"), ("GIT_AUTHOR_EMAIL", "test@example.invalid"),
                   ("GIT_COMMITTER_NAME", "test"), ("GIT_COMMITTER_EMAIL", "test@example.invalid")):
    os.environ.setdefault(key, value)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:800])


def tool(root, name, *args):
    return subprocess.run([sys.executable, os.path.join(root, "tools", name), *args], capture_output=True, text=True, cwd=root)


work = tempfile.mkdtemp(prefix="hakios-device-test-")
try:
    root = os.path.join(work, "root")
    os.makedirs(os.path.join(root, "tools"))
    for t in ("hakios-config.py", "hakios-device.py", "boot-args.py", "kernel-version.py"):
        shutil.copy(os.path.join(HERE, t), os.path.join(root, "tools", t))
    shutil.copytree(os.path.join(ROOT, "configs"), os.path.join(root, "configs"))
    # a SoC of the test's own, so that the test needs no SoC or device repository checked out (the common one alone runs it)
    os.makedirs(os.path.join(root, "socs", "fakesoc", "configs"))
    open(os.path.join(root, "socs", "fakesoc", "soc.conf"), "w").write("soc fakesoc\n\ttitle       a SoC made for the test\n")
    open(os.path.join(root, "socs", "fakesoc", "configs", "fakesoc.config"), "w").write(
        "# HakiOS fragment: the test's SoC.\nCONFIG_SOC=\"fakesoc\"\nCONFIG_CPUS=8\nCONFIG_THERMAL=y\nCONFIG_HAKIOS_THERMAL=y\n")
    for sub in os.listdir(os.path.join(ROOT, "drivers")):
        if os.path.isfile(os.path.join(ROOT, "drivers", sub, "conf")):
            os.makedirs(os.path.join(root, "drivers", sub))
            shutil.copy(os.path.join(ROOT, "drivers", sub, "conf"), os.path.join(root, "drivers", sub, "conf"))
    shutil.copytree(os.path.join(ROOT, "devices", "qemu"), os.path.join(root, "devices", "qemu"))

    r = tool(root, "hakios-device.py", "new", "fakephone", "--soc", "fakesoc", "--title", "Fake Phone")
    check("new: a device repository that check passes", r.returncode == 0 and "all hold together" in r.stdout, r.stdout + r.stderr)
    conf = open(os.path.join(root, "devices", "fakephone", "configs", "fakephone.config")).read()
    check("new: the phone drivers left out, the SoC's kept, QEMU's untouched",
          "# CONFIG_HAKIOS_GOODIX is not set" in conf and "HAKIOS_THERMAL" not in conf and "HAKIOS_VIRTIO" not in conf, conf)
    r = tool(root, "hakios-device.py", "new", "fakephone", "--soc", "fakesoc", "--title", "x")
    check("new: refuses a device that exists", r.returncode != 0 and "already exists" in r.stderr, r.stderr)
    check("new: the device carries its SoC's layer, a copy of the one there is",
          open(os.path.join(root, "devices", "fakephone", "soc", "soc.conf")).read() == open(os.path.join(root, "socs", "fakesoc", "soc.conf")).read())
    r = tool(root, "hakios-device.py", "new", "otherphone", "--soc", "newsoc", "--title", "Other Phone", "--repo")
    check("new: a SoC nobody has yet gets a new layer in the device, and --repo makes the device a git repository",
          r.returncode == 0 and os.path.isfile(os.path.join(root, "devices", "otherphone", "soc", "configs", "newsoc.config")) and
          os.path.isdir(os.path.join(root, "devices", "otherphone", ".git")) and "git -C devices/otherphone remote add origin" in r.stdout,
          r.stdout + r.stderr)
    shutil.rmtree(os.path.join(root, "devices", "otherphone"))
    # two copies of one SoC: the same is fine, different is refused
    with open(os.path.join(root, "devices", "fakephone", "soc", "configs", "fakesoc.config"), "a") as f:
        f.write("CONFIG_CPUS=4\n")
    r = tool(root, "hakios-config.py", "list")
    check("two different copies of one SoC are refused, with both named", r.returncode != 0 and "the SoC fakesoc is in" in r.stderr and
          "differ" in r.stderr, r.stdout + r.stderr)
    shutil.copy(os.path.join(root, "socs", "fakesoc", "configs", "fakesoc.config"), os.path.join(root, "devices", "fakephone", "soc", "configs"))
    shutil.rmtree(os.path.join(root, "socs"))
    r = tool(root, "hakios-config.py", "defconfig", "fakephone")
    check("a device that carries its SoC needs nothing else: no socs/ at all", r.returncode == 0, r.stdout + r.stderr)

    # a driver of the device's own
    d = os.path.join(root, "devices", "fakephone", "drivers", "input")
    os.makedirs(d)
    with open(os.path.join(d, "conf"), "w") as f:
        f.write("driver FAKETOUCH\n\tfile    bsd/dev/hakios.faketouch.c\n\thelp    the fake phone's touch controller\n")
    open(os.path.join(d, "hakios.faketouch.c"), "w").write("/* a touch driver */\n")
    with open(os.path.join(root, "devices", "fakephone", "configs", "fakephone.config"), "a") as f:
        f.write("CONFIG_HAKIOS_FAKETOUCH=y\n")
    r = tool(root, "hakios-config.py", "help", "HAKIOS_FAKETOUCH")
    check("a device's driver is a symbol like the common ones", r.returncode == 0 and "devices/fakephone/drivers/input/conf" in r.stdout, r.stdout + r.stderr)
    r = tool(root, "hakios-config.py", "external-files")
    check("external-files: its file, and where it goes in the tree",
          r.stdout.strip() == "%s\tbsd/dev/hakios.faketouch.c" % os.path.join(d, "hakios.faketouch.c"), r.stdout + r.stderr)
    r = tool(root, "hakios-config.py", "defconfig", "fakephone")
    check("make fakephone_defconfig: found in the device's repository, the driver built", r.returncode == 0 and
          "CONFIG_HAKIOS_FAKETOUCH=y" in open(os.path.join(root, ".config")).read(), r.stdout + r.stderr)
    tree = os.path.join(work, "tree")
    for sub in ("bsd/conf", "iokit/conf", "config", "bsd/dev"):
        os.makedirs(os.path.join(tree, sub))
    open(os.path.join(tree, "bsd/conf/files"), "w").write("bsd/dev/a.c standard\nbsd/dev/hakios.input.c standard\n")
    open(os.path.join(tree, "iokit/conf/files"), "w").write("iokit/Kernel/hakios.gpu.cpp optional iokitcpp\n")
    r = tool(root, "hakios-config.py", "kernel", "--tree", tree)
    files = open(os.path.join(tree, "bsd/conf/files")).read()
    check("kernel: the device's driver in conf/files and in the choice", r.returncode == 0 and
          "bsd/dev/hakios.faketouch.c" in files and "optional hakios_faketouch" in files and
          " hakios_faketouch " in open(os.path.join(tree, "config/MASTER.hakios")).read(), r.stdout + r.stderr + files)
    r = tool(root, "hakios-device.py", "check", "fakephone")
    check("check: the device with its driver holds together", r.returncode == 0, r.stdout + r.stderr)
    os.remove(os.path.join(d, "hakios.faketouch.c"))
    r = tool(root, "hakios-device.py", "check", "fakephone")
    check("check: a driver naming a file that is not there is a problem", r.returncode != 0 and "hakios.faketouch.c" in r.stdout, r.stdout)
    with open(os.path.join(d, "conf"), "a") as f:
        f.write("standard bsd/dev/hakios.everyone.c\n")
    r = tool(root, "hakios-config.py", "list")
    check("a device repository cannot declare `standard` files", "a SoC or device repository has drivers, not `standard`" in r.stdout + r.stderr,
          r.stdout + r.stderr)
    os.remove(os.path.join(d, "conf"))
    shutil.copy(os.path.join(root, "devices", "fakephone", "soc", "configs", "fakesoc.config"),
                os.path.join(root, "devices", "fakephone", "configs", "fakesoc.config"))
    r = tool(root, "hakios-config.py", "defconfig", "fakephone")
    check("the same configuration name in two repositories is refused", r.returncode != 0 and "more than one repository" in r.stderr, r.stderr)

    # the real tree
    r = tool(ROOT, "hakios-device.py", "check")
    check("the devices and SoCs of this tree hold together", r.returncode == 0, r.stdout + r.stderr)
    r = tool(ROOT, "hakios-device.py", "list")
    check("list names the QEMU device, which the common repository always has", "qemu" in r.stdout, r.stdout)
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("hakios-device-test: %d FAILED" % len(failures))
print("hakios-device-test: all passed")
