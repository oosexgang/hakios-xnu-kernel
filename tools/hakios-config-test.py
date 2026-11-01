#!/usr/bin/env python3
"""tools/hakios-config.py: what the configurations of configs/ give, and what the tool refuses.

  the real configs/   redwood gives exactly the arguments the project's phone line used before there were configurations
                      (so choosing it changes nothing); redwood + open loads nothing of the phone's own; persist adds the
                      writes to `darwin`; virt and trace do what they say; kernel.ver carries -sc7280
  refusals            an unknown symbol (with a suggestion), a firmware option in the open profile, a dependency that is
                      missing, a number out of range, a bad hex or string, a command line that cannot fit
  drivers             CONFIG_HAKIOS_* from drivers/*/conf: built in unless a configuration says otherwise; `is not set` leaves one
                      out; a driver without one it requires, and an argument only a left-out driver reads, are refused; `kernel`
                      writes the choice into a kernel tree where XNU's config reads it, and writes nothing when nothing changed
  workflow            merge over an existing .config; `is not set` in a fragment switches off; savedefconfig round-trips;
                      --config and --root work after the command as well as before it
"""
import os
import shutil
import subprocess
import sys
import tempfile
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "hakios-config.py")
failures = []

# what `make phone` passed before there were configurations: the runner's four, then the Makefile's ARGS
OLD_PHONE_LINE = ("bootcpu=7 cpus=8 cpufreq=1 ufs=2 touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 modem=1 modem_again=1 "
                  "modem_buffer=0xa8000000").split()


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:500])


def run(*args, root=ROOT):
    return subprocess.run([sys.executable, TOOL, "--root", root, *args], capture_output=True, text=True)


def args_of(out):
    return out.stdout.split()


# The reference phone's configuration is what most of this checks against: its repositories (devices/redwood, with its SoC layer) are
# their own, and a checkout of the common repository alone does not have them until `tools/hakios-device.py sync` clones them.
if not (os.path.isdir(os.path.join(ROOT, "devices", "redwood", "configs")) and os.path.isdir(os.path.join(ROOT, "devices", "redwood", "soc", "configs"))):
    print("hakios-config-test: SKIPPED: it checks the reference phone's configuration, and devices/redwood is not "
          "checked out (tools/hakios-device.py sync redwood)")
    sys.exit(0)

work = tempfile.mkdtemp(prefix="hakios-config-test-")
try:
    cfg = os.path.join(work, "cfg")

    r = run("defconfig", "redwood", "-o", cfg)
    check("defconfig redwood: makes the .config", r.returncode == 0 and os.path.isfile(cfg), r.stdout + r.stderr)
    a = args_of(run("args", "--config", cfg))
    # the phone line from before there were configurations, plus the two things the SC7280 fragment turns on (the chip's identity
    # and the sensors) and the two of redwood.config's panel (120 Hz frames, CoreAnimation clocked by the driver)
    # and redwood.config's four big cores (cpus=4) instead of the line's eight
    PANEL = ["fb_hz=120", "ca_vbl=1"]
    want = ["cpus=4" if x == "cpus=8" else x for x in OLD_PHONE_LINE] + ["socinfo=1", "thermal=1"] + PANEL
    check("redwood: the project's phone line before configurations, plus socinfo and thermal, the panel's 120 Hz and the big cores",
          sorted(a) == sorted(want), "got %s\nwant %s" % (sorted(a), sorted(want)))
    check("redwood: kernel.ver carries the SoC", "-sc7280-" in r.stdout, r.stdout)
    check("redwood: the .config is in Linux's form",
          "CONFIG_GPU=y" in open(cfg).read() and "# CONFIG_CAM is not set" in open(cfg).read() and 'CONFIG_FIRMWARE="device"' in open(cfg).read())
    check("redwood: check passes", run("check", "--config", cfg).returncode == 0)

    r = run("needs", "--config", cfg)
    check("redwood: needs says which firmware, and that it is missing here (none is pulled in a clean checkout)",
          "fw-gpu/" in r.stdout and "fw-modem/" in r.stdout, r.stdout)

    # the open profile over redwood
    o = os.path.join(work, "open")
    r = run("defconfig", "redwood", "open", "-o", o)
    check("redwood + open: passes", r.returncode == 0, r.stdout + r.stderr)
    check("redwood + open: nothing of the phone's own is loaded",
          sorted(args_of(run("args", "--config", o))) == sorted(("cpus=4 bootcpu=7 cpufreq=1 ufs=2 touchscreen=1 socinfo=1 thermal=1".split() + PANEL)),
          run("args", "--config", o).stdout)
    r = run("needs", "--config", o)
    check("redwood + open: needs nothing", r.returncode == 0 and "nothing of the phone's own" in r.stdout, r.stdout)

    # fragments
    p = os.path.join(work, "persist")
    shutil.copyfile(cfg, p)
    r = run("merge", "persist", "--config", p)
    check("merge persist: ufs_write and var_store appear", r.returncode == 0 and {"ufs_write=1", "var_store=1"} <= set(args_of(run("args", "--config", p))), r.stdout + r.stderr)
    check("merge persist: UFS stays 2, not duplicated", args_of(run("args", "--config", p)).count("ufs=2") == 1)
    v = os.path.join(work, "virt")
    run("defconfig", "defconfig", "virt", "-o", v)
    check("defconfig + virt: virtio_net, one core, no firmware", sorted(args_of(run("args", "--config", v))) == ["cpus=1", "virtio_net=1"],
          run("args", "--config", v).stdout)
    d = os.path.join(work, "base")
    run("defconfig", "defconfig", "-o", d)
    check("defconfig alone is the open one: cpus=1", args_of(run("args", "--config", d)) == ["cpus=1"], run("args", "--config", d).stdout)
    t = os.path.join(work, "trace")
    run("defconfig", "redwood", "trace", "-o", t)
    check("trace: net_trace=20", "net_trace=20" in args_of(run("args", "--config", t)))

    # the arguments straight from a defconfig, with no .config made (what `make qemu-open` asks for)
    check("args --defconfig defconfig: cpus=1", args_of(run("args", "--defconfig", "defconfig")) == ["cpus=1"], run("args", "--defconfig", "defconfig").stdout)
    check("args --defconfig redwood --fragment open: the open phone",
          sorted(args_of(run("args", "--defconfig", "redwood", "--fragment", "open"))) == sorted(("cpus=4 bootcpu=7 cpufreq=1 ufs=2 touchscreen=1 socinfo=1 thermal=1".split() + PANEL)))
    check("args --defconfig redwood: the same as the .config's", sorted(args_of(run("args", "--defconfig", "redwood"))) == sorted(a))

    # the order of the arguments is the table's: cpus first, as in the README's lines
    check("the arguments come in the order of the symbol table", args_of(run("args", "--config", cfg))[:3] == ["cpus=4", "bootcpu=7", "cpufreq=1"])

    # savedefconfig round trip: the minimal defconfig, made a defconfig of its own, gives the same arguments
    sd = run("savedefconfig", "--config", cfg)
    check("savedefconfig: drops what defconfig already has", "CONFIG_FIRMWARE=\"none\"" not in sd.stdout and "CONFIG_GPU=y" in sd.stdout, sd.stdout)
    fake = os.path.join(work, "root")
    shutil.copytree(os.path.join(ROOT, "configs"), os.path.join(fake, "configs"))
    open(os.path.join(fake, "configs", "mine_defconfig"), "w").write(sd.stdout)
    shutil.copyfile(os.path.join(ROOT, "VERSION"), os.path.join(fake, "VERSION"))
    r = run("defconfig", "mine", "-o", os.path.join(work, "mine"), root=fake)
    check("savedefconfig round trip: same arguments", sorted(args_of(run("args", "--config", os.path.join(work, "mine"), root=fake))) == sorted(a), r.stdout + r.stderr)

    # `--config` and `--root` after the command too (checked above for --config); before it:
    check("--config before the command works too", run("--config", cfg, "check").returncode == 0)

    # refusals, one configuration each in a temporary configs/
    def refusal(label, text, want, base_text=None):
        d = os.path.join(work, "r%d" % len(os.listdir(work)))
        os.makedirs(os.path.join(d, "configs"))
        open(os.path.join(d, "configs", "defconfig"), "w").write(base_text if base_text is not None else 'CONFIG_FIRMWARE="device"\nCONFIG_CPUS=1\n')
        open(os.path.join(d, "configs", "bad_defconfig"), "w").write(text)
        shutil.copyfile(os.path.join(ROOT, "VERSION"), os.path.join(d, "VERSION"))
        r = run("defconfig", "bad", "-o", os.path.join(d, "out"), root=d)
        out = r.stdout + r.stderr
        check(label, r.returncode != 0 and want in out, "exit %d, want %r in:\n%s" % (r.returncode, want, out))

    refusal("an unknown symbol is refused, with a suggestion", "CONFIG_WIFFI=y\n", "did you mean CONFIG_WIFI")
    refusal("a line that is no setting is refused, with its number", "this is not a setting\n", "bad_defconfig:1")
    refusal("a firmware option in the open profile is refused", 'CONFIG_FIRMWARE="none"\nCONFIG_GPU=y\n', "needs the phone's own firmware")
    refusal("a missing dependency is refused (modem without ufs=2)", "CONFIG_MODEM=y\nCONFIG_UFS=1\n", "requires CONFIG_UFS=2")
    refusal("var_store without ufs_write is refused", "CONFIG_VAR_STORE=y\n", "requires CONFIG_UFS_WRITE")
    refusal("cpus out of range", "CONFIG_CPUS=9\n", "CONFIG_CPUS is 1 to 8")
    refusal("a hex that is not hex", "CONFIG_UFS=2\nCONFIG_MODEM=y\nCONFIG_MODEM_BUFFER=a8000000\n", "starts with 0x")
    refusal("a bool that is not y", "CONFIG_GPU=1\n", "is a bool: y, or not set")
    refusal("a string without quotes", "CONFIG_DEVICE=redwood\n", "string in quotes")
    refusal("a FIRMWARE that is neither", 'CONFIG_FIRMWARE="maybe"\n', '"device" or "none"')

    r = run("defconfig", "nosuchphone", "-o", os.path.join(work, "x"))
    check("an unknown defconfig lists what there is", r.returncode != 0 and "redwood_defconfig" in (r.stdout + r.stderr), r.stdout + r.stderr)
    r = run("args", "--config", os.path.join(work, "nothing"))
    check("no .config: says how to make one", r.returncode != 0 and "defconfig NAME" in (r.stdout + r.stderr), r.stdout + r.stderr)

    # `=n` for a bool is `is not set`; a later `is not set` removes what an earlier file set
    fake2 = os.path.join(work, "root2")
    os.makedirs(os.path.join(fake2, "configs"))
    shutil.copyfile(os.path.join(ROOT, "VERSION"), os.path.join(fake2, "VERSION"))
    open(os.path.join(fake2, "configs", "defconfig"), "w").write("CONFIG_CPUS=2\nCONFIG_CPUFREQ=y\n")
    open(os.path.join(fake2, "configs", "x_defconfig"), "w").write("CONFIG_CPUFREQ=n\nCONFIG_TOUCHSCREEN=y\n")
    open(os.path.join(fake2, "configs", "y.config"), "w").write("# CONFIG_TOUCHSCREEN is not set\n")
    out2 = os.path.join(work, "o2")
    run("defconfig", "x", "y", "-o", out2, root=fake2)
    check("=n and `is not set` switch a symbol off", args_of(run("args", "--config", out2, root=fake2)) == ["cpus=2"], run("args", "--config", out2, root=fake2).stdout)

    # the command line that cannot fit: the table is short, so shrink the limit and ask the checker
    spec = importlib.util.spec_from_file_location("hc", TOOL)
    hc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hc)
    hc.KERNEL_LINE = 300
    problems, _ = hc.check({"CPUS": "8", "BOOTCPU": "7", "UFS": "2", "MODEM": "y", "MODEM_BUFFER": "0xa8000000", "FIRMWARE": '"device"'})
    check("a command line that cannot fit is a problem", any("the end of it would be lost" in p for p in problems), problems)

    # CONFIG_BOOTARGS: Linux's CMDLINE, but every name is checked against what the source reads
    hc.KERNEL_LINE = 607
    base = {"CPUS": "4", "FIRMWARE": '"device"'}
    problems, a = hc.check(dict(base, BOOTARGS='"fb_beat=2 loader_return_after=5"'))
    check("BOOTARGS: names the source reads are fine, and come after the symbols' own", not problems and a == ["cpus=4", "fb_beat=2", "loader_return_after=5"], (problems, a))
    problems, _ = hc.check(dict(base, BOOTARGS='"fb_beet=2"'))
    check("BOOTARGS: a name nothing reads is an error, with a suggestion", any("nothing reads the argument fb_beet" in p and "did you mean fb_beat" in p for p in problems), problems)
    problems, _ = hc.check(dict(base, BOOTARGS='"cpus=8"'))
    check("BOOTARGS: an argument a symbol already sets is refused", any("already set by CONFIG_CPUS" in p for p in problems), problems)
    problems, _ = hc.check(dict(base, BOOTARGS='"fb_beat=2 fb_beat=3"'))
    check("BOOTARGS: the same name twice is refused", any("fb_beat twice" in p for p in problems), problems)
    for bad in ('"fb_beat"', '"fb_beat=a b"', '"fb_beat=\\"x"', '"FB_BEAT=1"'):
        problems, _ = hc.check(dict(base, BOOTARGS=bad))
        check("BOOTARGS: %s is not name=value" % bad, any("is not name=value" in p or "not name=value" in p for p in problems), problems)
    problems, _ = hc.check(dict(base, BOOTARGS="fb_beat=2"))
    check("BOOTARGS without quotes is refused", any("string in quotes" in p for p in problems), problems)
    long = " ".join("modem_ipa_data_trace=%d" % i for i in range(30))
    problems, _ = hc.check(dict(base, BOOTARGS='"%s"' % long))
    check("BOOTARGS counts toward the length limit", any("the end of it would be lost" in p for p in problems), problems)

    # the common kernel's configuration: not named after a phone, and a phone's defconfig is only what it includes
    r = run("defconfig", "common", "-o", os.path.join(work, "common"))
    check("common_defconfig passes its own checks", r.returncode == 0, r.stdout + r.stderr)
    check("common names every symbol there is", run("complete", "common").returncode == 0, run("complete", "common").stdout)
    listed = run("listing", "common").stdout
    with open(os.path.join(ROOT, "configs", "common_defconfig"), encoding="utf-8", newline="") as f:
        check("common_defconfig is what `listing common` makes of it (nothing edited by hand drifted)",
              f.read().replace("\r\n", "\n") == listed, "run: tools/hakios-config.py listing common -o configs/common_defconfig")
    check("the listing has hundreds of symbols, each with a comment above it",
          listed.count("\nCONFIG_") + listed.count("\n# CONFIG_") >= 300 and listed.count("\n# ") > 600, listed.count("\n"))
    with open(os.path.join(ROOT, "devices", "redwood", "configs", "redwood_defconfig"), encoding="utf-8") as f:
        includes = [l.split()[1] for l in f if l.startswith("#include ")]
    check("redwood_defconfig is the common one, the SoC's and the phone's, put together", includes == ["common_defconfig", "sc7280.config", "redwood.config"], includes)
    check("the common configuration does not say which phone: no device, no SoC",
          not any(l.startswith(("CONFIG_DEVICE=", "CONFIG_SOC=")) for l in listed.splitlines()))
    # #include: a fragment can put others first; a loop is an error, not a hang
    fake3 = os.path.join(work, "fake3")
    os.makedirs(os.path.join(fake3, "configs"))
    for name, text in (("defconfig", 'CONFIG_FIRMWARE="none"\n'), ("a.config", "#include b.config\nCONFIG_CPUS=2\n"), ("b.config", "CONFIG_CPUS=4\nCONFIG_BOOTCPU=1\n"),
                       ("x.config", "#include y.config\n"), ("y.config", "#include x.config\n")):
        with open(os.path.join(fake3, "configs", name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    out3 = os.path.join(work, "out3")
    r = run("defconfig", "defconfig", "a", "-o", out3, root=fake3)
    check("#include: the included file is laid first, the including one wins", args_of(run("args", "--config", out3, root=fake3)) == ["cpus=2", "bootcpu=1"],
          run("args", "--config", out3, root=fake3).stdout + r.stderr)
    r = run("defconfig", "defconfig", "x", "-o", out3, root=fake3)
    check("#include: a loop is refused", r.returncode != 0 and "includes itself" in (r.stdout + r.stderr), r.stdout + r.stderr)
    # the generated symbols: any argument the source reads, typed and checked
    hc.SYMBOLS  # (loaded above)
    problems, a = hc.check({"ADRENO_KEEP": "y", "FB_BEAT": "2", "APFS_SKIP": '"/a,/b"', "FIRMWARE": '"device"'})
    check("a generated symbol: y is =1, a number is as it is, a quoted text is the text", not problems and a == ["adreno_keep=1", "apfs_skip=/a,/b", "fb_beat=2"], (problems, a))     # (the generated ones, by name)
    problems, _ = hc.check({"FB_BEAT": "fast"})
    check("a generated symbol: a word that is none of those is an error", any("CONFIG_FB_BEAT is y, a number" in p for p in problems), problems)
    problems, _ = hc.check({"FB_BEAT": '"a b"'})
    check("a generated symbol: a text with a space in it is an error", any("CONFIG_FB_BEAT" in p for p in problems), problems)
    problems, _ = hc.check({"THERMAL_STOP": "105"})
    check("THERMAL_STOP requires THERMAL", any("requires CONFIG_THERMAL" in p for p in problems), problems)
    problems, _ = hc.check({"THERMAL": "y", "THERMAL_STOP": "500"})
    check("THERMAL_STOP is at most 160 degrees", any("THERMAL_STOP" in p for p in problems), problems)
    check("there are as many symbols as arguments the source reads, less the ones the tools set",
          len(hc.SYMBOLS) >= 300 and len({s[0] for s in hc.SYMBOLS}) == len(hc.SYMBOLS), len(hc.SYMBOLS))

    # the drivers: compiled in, not arguments
    check("redwood builds every driver, and they give no argument",
          all("CONFIG_%s=y" % d.symbol in open(cfg).read() for d in hc.DRIVERS) and len(hc.DRIVERS) >= 20 and
          not any(x.startswith("hakios_") for x in args_of(run("args", "--config", cfg))), len(hc.DRIVERS))
    built = hc.drivers_built({})
    check("a configuration that names no driver builds them all (their default)", all(built.values()), built)
    off = dict(hc.parse(cfg)[0])
    problems, _ = hc.check(dict(off, HAKIOS_ADSP="n"))
    check("a driver without one it requires is refused (SOUND without ADSP)", any("CONFIG_HAKIOS_SOUND requires CONFIG_HAKIOS_ADSP" in p for p in problems), problems)
    problems, _ = hc.check(dict(off, HAKIOS_GOODIX="n"))
    check("an argument another built driver still reads is fine (touchscreen= is the buttons' too)", not problems, problems)
    problems, _ = hc.check(dict(off, HAKIOS_GOODIX="n", HAKIOS_BUTTONS="n"))
    check("an argument only left-out drivers read is refused (touchscreen= without GOODIX and BUTTONS)",
          any("CONFIG_TOUCHSCREEN gives touchscreen=" in p and "CONFIG_HAKIOS_GOODIX" in p and "CONFIG_HAKIOS_BUTTONS" in p for p in problems), problems)
    problems, _ = hc.check(dict(off, HAKIOS_GOODIX="n", HAKIOS_BUTTONS="n", TOUCHSCREEN="n"))
    check("... and with that argument gone too, the choice passes", not [p for p in problems if "GOODIX" in p or "touchscreen" in p], problems)
    problems, _ = hc.check(dict(off, HAKIOS_GOODIX="maybe"))
    check("a driver is y or not set", any("CONFIG_HAKIOS_GOODIX is a driver" in p for p in problems), problems)
    # a phone without that touch controller: a fragment in a root of its own (merge takes fragments of configs/)
    fake4 = os.path.join(work, "fake4")
    shutil.copytree(os.path.join(ROOT, "configs"), os.path.join(fake4, "configs"))
    with open(os.path.join(fake4, "configs", "nogoodix.config"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# no Goodix here\n# CONFIG_HAKIOS_GOODIX is not set\n")
    nog = os.path.join(work, "nogoodix")
    shutil.copyfile(cfg, nog)
    r = run("merge", "nogoodix", "--config", nog, root=fake4)
    check("`# CONFIG_HAKIOS_GOODIX is not set` in a fragment leaves the driver out", r.returncode == 0 and
          "# CONFIG_HAKIOS_GOODIX is not set" in open(nog).read() and "CONFIG_HAKIOS_BUTTONS=y" in open(nog).read(), r.stdout + r.stderr)
    r = run("savedefconfig", "--config", nog, root=fake4)
    check("savedefconfig keeps a driver that is left out", "# CONFIG_HAKIOS_GOODIX is not set" in r.stdout and "CONFIG_HAKIOS_BUTTONS" not in r.stdout, r.stdout)
    r = run("help", "HAKIOS_SOUND")
    check("help on a driver: its files, what it requires, what requires it",
          "hakios.sound.c" in r.stdout and "requires: CONFIG_HAKIOS_ADSP, CONFIG_HAKIOS_SPEAKER" in r.stdout and "CONFIG_HAKIOS_MIC" in r.stdout, r.stdout)
    # `kernel`: into a tree that has what XNU's config reads, as apply.sh leaves it before the first run (loose hakios lines)
    tree = os.path.join(work, "tree")
    for d in ("bsd/conf", "iokit/conf", "config", "bsd/dev"):
        os.makedirs(os.path.join(tree, d))
    with open(os.path.join(tree, "bsd/conf/files"), "w") as f:
        f.write("bsd/dev/a.c standard\nbsd/dev/hakios.input.c\t\tstandard\nbsd/dev/hakios.goodix.c\tstandard\nbsd/dev/z.c standard\n")
    with open(os.path.join(tree, "iokit/conf/files"), "w") as f:
        f.write("iokit/Kernel/A.cpp optional iokitcpp\niokit/Kernel/hakios.gpu.cpp optional iokitcpp\n")
    r = run("kernel", "--tree", tree, "--config", nog)
    master = open(os.path.join(tree, "config/MASTER.hakios")).read()
    files = open(os.path.join(tree, "bsd/conf/files")).read()
    check("kernel: the choice is a list of attributes doconf expands, and an `options` line for every driver",
          r.returncode == 0 and "#  HAKIOS_DRIVERS = [ " in master and " hakios_buttons " in master and " hakios_goodix " not in master and
          "options\t\tHAKIOS_GOODIX" in master and "# <hakios_goodix>" in master, r.stdout + r.stderr + master)
    check("kernel: conf/files has one block, a driver's files `optional hakios_<name>`, the rest of the file as it was",
          files.startswith("bsd/dev/a.c standard\n" + hc.FILES_BEGIN) and files.rstrip().endswith("bsd/dev/z.c standard") and
          "bsd/dev/hakios.goodix.c                  optional hakios_goodix" in files and "bsd/dev/hakios.input.c                   standard" in files and
          "iokit/Kernel/hakios.gpu.cpp              optional hakios_gpu iokitcpp" in open(os.path.join(tree, "iokit/conf/files")).read(), files)
    listed_line = [l for l in open(os.path.join(tree, "bsd/dev/hakios.drivers.h")) if l.startswith("#define HAKIOS_DRIVERS_LIST")]
    check("kernel: the header kern.hakios.drivers comes from", listed_line and " buttons " in listed_line[0] and "goodix" not in listed_line[0], listed_line)
    r = run("kernel", "--tree", tree, "--config", nog)
    check("kernel: run again, it writes nothing (an unchanged choice rebuilds nothing)", r.returncode == 0 and "written" not in r.stdout, r.stdout)
    r = run("kernel", "--tree", tree, "--config", os.path.join(work, "nosuch"))
    check("kernel: without a .config every driver is built", " hakios_goodix " in open(os.path.join(tree, "config/MASTER.hakios")).read(), r.stdout + r.stderr)
    bad = os.path.join(work, "badchoice")
    shutil.copyfile(nog, bad)
    with open(bad, "a") as f:
        f.write("# CONFIG_HAKIOS_ADSP is not set\n")
    r = run("kernel", "--tree", tree, "--config", bad)
    check("kernel: refuses a choice a driver's requires breaks", r.returncode != 0 and "requires CONFIG_HAKIOS_ADSP" in r.stdout, r.stdout + r.stderr)

    # help, show, list
    r = run("help", "gpu")
    check("help GPU: the argument, the firmware", "gpu=1" in r.stdout and "fw-gpu/" in r.stdout, r.stdout)
    r = run("help", "CONFIG_MODEM_BUFFER")
    check("help takes CONFIG_ and says what it requires", r.returncode == 0 and "requires: CONFIG_MODEM" in r.stdout, r.stdout)
    r = run("help", "gpuu")
    check("help on an unknown symbol suggests", r.returncode != 0 and "did you mean CONFIG_GPU" in r.stdout, r.stdout)
    r = run("list")
    check("list: defconfigs and fragments", all(x in r.stdout for x in ("redwood", "open", "virt", "persist", "trace")), r.stdout)
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("hakios-config-test: %d FAILED" % len(failures))
print("hakios-config-test: all passed")
