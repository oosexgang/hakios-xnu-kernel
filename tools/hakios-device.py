#!/usr/bin/env python3
"""HakiOS device and SoC repositories: the common repository (this one), a repository per SoC family, one per device.

The way Android puts a phone's tree together (device/<vendor>/<name>, a <soc>-common, a manifest that names them) and the way XNU
ports keep a board apart from the kernel: what is true of every device lives here (the kernel, the loader, the drivers, the tools);
what is true of a SoC lives in socs/<soc>/; what is true of one device lives in devices/<name>/. Each of those two is a directory of
the same shape that may be its own git repository:

    devices/<name>/device.conf          what the device is, and how far each part of it works (the device's page is made from it)
    devices/<name>/configs/             <name>_defconfig (#include common_defconfig, the SoC's fragment, <name>.config) and fragments
    devices/<name>/drivers/<sub>/conf   drivers of its own, if it has any, in the format of drivers/<sub>/conf (docs/drivers-config.md)
    socs/<soc>/soc.conf, configs/<soc>.config, drivers/...    the same for a SoC family

devices/manifest.conf names every one: `path` for one kept in this tree, `repository` and `revision` for one that is its own
repository, which `sync` clones into place at that revision.

  tools/hakios-device.py list                       the devices and SoCs, where each is, how far each part works
  tools/hakios-device.py check [NAME...]            a device or SoC repository holds together (CI runs it on every one)
  tools/hakios-device.py new NAME --soc SOC ...     a new device repository, with every phone driver left out to begin with
  tools/hakios-device.py sync [NAME...]             clone or update the repositories the manifest names, at their revisions
  tools/hakios-device.py lock NAME                  write the revision checked out into the manifest
  tools/hakios-device.py split NAME DIR             a device kept in this tree as a git repository of its own, with its history

docs/porting/repositories.md is the guide.
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATES = ("works", "partial", "missing")
AREAS = ("display", "touch", "buttons", "cpu", "gpu", "storage", "audio", "battery", "haptic", "camera", "torch", "wifi",
         "bluetooth", "cellular", "network", "thermal", "usb", "sensors")
DEVICE_KEYS = {"title", "vendor", "soc", "defconfig", "firmware", "maintainer", "status", "repository", "revision", "path"}
SOC_KEYS = {"title", "vendor", "cores", "gpu", "maintainer", "repository", "revision", "path"}


class Problem(Exception):
    pass


_CONFIG = []


def config_tool():
    if _CONFIG:
        return _CONFIG[0]
    spec = importlib.util.spec_from_file_location("hakios_config", os.path.join(HERE, "hakios-config.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _CONFIG.append(module)
    return module


def read_blocks(path, kinds, keys):
    """[(kind, name, {key: [values]}, line)] of a file of blocks: `<kind> <name>` and indented `<key> <value>` lines under it."""
    blocks, block = [], None
    shown = os.path.relpath(path, ROOT)
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        words = line.split()
        if not raw[0].isspace():
            if words[0] not in kinds or len(words) != 2 or not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", words[1]):
                raise Problem("%s:%d: want `%s NAME` (lowercase), got %r" % (shown, n, " NAME` or `".join(kinds), line.strip()))
            block = (words[0], words[1], {}, n)
            blocks.append(block)
            continue
        if block is None:
            raise Problem("%s:%d: an indented line before any block" % (shown, n))
        key, value = words[0], line.strip()[len(words[0]):].strip()
        allowed = keys[block[0]]
        if key not in allowed:
            raise Problem("%s:%d: %s does not take `%s` (it takes: %s)" % (shown, n, block[0], key, ", ".join(sorted(allowed))))
        block[2].setdefault(key, []).append((value, n))
    return blocks


def one(fields, key, default=None):
    values = fields.get(key)
    return values[-1][0] if values else default


def manifest():
    path = os.path.join(ROOT, "devices", "manifest.conf")
    if not os.path.isfile(path):
        return []
    return read_blocks(path, ("device", "soc", "uefi"), {"device": DEVICE_KEYS, "soc": SOC_KEYS, "uefi": {"path", "repository", "revision"}})


def soc_homes():
    """{SoC name: its directory}: socs/<name>/, then the SoC layers devices carry (devices/<device>/soc/), the first of each."""
    out = {}
    base = os.path.join(ROOT, "socs")
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if os.path.isfile(os.path.join(base, name, "soc.conf")):
            out.setdefault(name, os.path.join(base, name))
    base = os.path.join(ROOT, "devices")
    for dev in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        top = os.path.join(base, dev, "soc")
        named = config_tool().soc_name(top)
        if named:
            out.setdefault(named, top)
    return out


def home(kind, name):
    if kind == "uefi":
        return os.path.join(ROOT, "uefi")         # one UEFI for every device: Mu-Silicium with HakiOS's changes (uefi/README.md)
    if kind == "soc":
        return soc_homes().get(name, os.path.join(ROOT, "socs", name))
    return os.path.join(ROOT, "devices", name)


def describe(kind, name):
    """The block of devices/<name>/device.conf or socs/<name>/soc.conf, or None if it is not checked out."""
    path = os.path.join(home(kind, name), "device.conf" if kind == "device" else "soc.conf")
    if not os.path.isfile(path):
        return None
    blocks = read_blocks(path, (kind,), {"device": DEVICE_KEYS, "soc": SOC_KEYS})
    if len(blocks) != 1 or blocks[0][1] != name:
        raise Problem("%s: want one `%s %s` block" % (os.path.relpath(path, ROOT), kind, name))
    return blocks[0][2]


def present():
    """[(kind, name)] of every device and SoC checked out, whether or not the manifest names it."""
    out = [("soc", name) for name in sorted(soc_homes())]
    base = os.path.join(ROOT, "devices")
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if os.path.isfile(os.path.join(base, name, "device.conf")):
            out.append(("device", name))
    return out


def statuses(fields):
    """[(area, state, note)] of a device's `status` lines, checked."""
    out = []
    for value, n in fields.get("status", []):
        parts = value.split(None, 2)
        if len(parts) < 2 or parts[0] not in AREAS or parts[1] not in STATES:
            raise Problem("line %d: want `status <area> works|partial|missing <note>`, the area one of %s (got %r)" %
                          (n, ", ".join(AREAS), value))
        out.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    return out


# ---- commands ----------------------------------------------------------------------------------------------------

def cmd_list(_a):
    named = {(k, n): f for k, n, f, _l in manifest()}
    every = sorted(set(named) | set(present()), key=lambda x: (x[0] != "soc", x[1]))
    for kind, name in every:
        if kind == "uefi":
            where = one(named[(kind, name)], "repository") or one(named[(kind, name)], "path")
            there = os.path.isfile(os.path.join(home(kind, name), "apply.sh"))
            print("%-6s %-10s %-44s %s" % (kind, name, "HakiOS UEFI (Mu-Silicium)" if there else
                                            "NOT CHECKED OUT (tools/hakios-device.py sync %s)" % name, where))
            continue
        fields = describe(kind, name)
        where = one(named.get((kind, name), {}), "repository") or one(named.get((kind, name), {}), "path")
        if not where and kind == "soc" and os.path.basename(home(kind, name)) == "soc":
            where = "carried by %s" % os.path.relpath(os.path.dirname(home(kind, name)), ROOT)
        where = where or "(not in the manifest)"
        if fields is None:
            print("%-6s %-10s NOT CHECKED OUT (tools/hakios-device.py sync %s)  %s" % (kind, name, name, where))
            continue
        line = "%-6s %-10s %-44s %s" % (kind, name, one(fields, "title", "?")[:44], where)
        if kind == "device":
            counts = {s: 0 for s in STATES}
            for _area, state, _note in statuses(fields):
                counts[state] += 1
            line += "  [%d works, %d partial, %d missing]" % (counts["works"], counts["partial"], counts["missing"])
        print(line)
    return 0


def check_one(kind, name, hc):
    problems = []
    try:
        fields = describe(kind, name)
    except Problem as e:
        return [str(e)]
    if fields is None:
        return ["%s %s is not checked out" % (kind, name)]
    top = home(kind, name)
    shown = os.path.relpath(top, ROOT)
    if not one(fields, "title"):
        problems.append("%s: no title" % shown)
    if kind == "device":
        try:
            areas = statuses(fields)
        except Problem as e:
            problems.append("%s/device.conf: %s" % (shown, e))
            areas = []
        if len({a for a, _s, _n in areas}) != len(areas):
            problems.append("%s/device.conf: an area has two status lines" % shown)
        soc = one(fields, "soc")
        if soc is None:
            problems.append("%s/device.conf: no soc (`none` for a machine with no SoC repository)" % shown)
        elif soc != "none" and soc not in soc_homes():
            problems.append("%s/device.conf: soc %s, and neither %s/soc/ nor socs/%s/ has its soc.conf" % (shown, soc, shown, soc))
        defconfig = one(fields, "defconfig")
        path = os.path.join(top, "configs", defconfig or "")
        if not defconfig or not os.path.isfile(path):
            problems.append("%s/device.conf: defconfig %s is not in %s/configs/" % (shown, defconfig, shown))
        else:
            includes = [l.split()[1] for l in open(path, encoding="utf-8") if l.startswith("#include ")]
            if "common_defconfig" not in includes:
                problems.append("%s/configs/%s does not #include common_defconfig: a device is the common kernel plus its own" % (shown, defconfig))
            if soc not in (None, "none") and soc + ".config" not in includes:
                problems.append("%s/configs/%s does not #include %s.config (its SoC's facts)" % (shown, defconfig, soc))
            out = subprocess.run([sys.executable, os.path.join(HERE, "hakios-config.py"), "defconfig", defconfig, "-o", os.devnull],
                                 capture_output=True, text=True, cwd=ROOT)
            for l in (out.stdout + out.stderr).splitlines():
                if "PROBLEM" in l or out.returncode != 0 and l.startswith("hakios-config:") and "made from" not in l and "characters" not in l:
                    problems.append("%s/configs/%s: %s" % (shown, defconfig, l.replace("hakios-config: ", "")))
        firmware = one(fields, "firmware")
        if firmware and not os.path.isfile(os.path.join(top, firmware)):
            problems.append("%s/device.conf: firmware %s is not there" % (shown, firmware))
    else:
        fragment = os.path.join(top, "configs", name + ".config")
        if not os.path.isfile(fragment):
            problems.append("%s: no configs/%s.config, the fragment a device with this SoC includes" % (shown, name))
    for d in hc.DRIVERS:
        if d.origin and os.path.abspath(d.origin) == os.path.abspath(top):
            for f, _tags in d.files:
                if not os.path.isfile(d.source(f)):
                    problems.append("%s: the driver %s names %s, which is not there" % (d.where, d.name, os.path.relpath(d.source(f), ROOT)))
    return problems


def cmd_check(a):
    hc = config_tool()
    if hc.DRIVERS_ERROR:
        print("hakios-device: " + hc.DRIVERS_ERROR)
        return 1
    every = present()
    wanted = [(k, n) for k, n in every if not a.names or n in a.names]
    missing = [n for n in a.names if n not in {x[1] for x in every}]
    problems = ["%s: not checked out here" % n for n in missing]
    for kind, name in wanted:
        problems += check_one(kind, name, hc)
    names = {}
    for kind, name in every:
        if name in names:
            problems.append("%s is both a %s and a %s: a configuration name would be ambiguous" % (name, names[name], kind))
        names[name] = kind
    for p in problems:
        print("hakios-device: " + p)
    print("hakios-device: %d repositories checked (%s)%s" % (len(wanted), ", ".join(n for _k, n in wanted),
          ", %d problem(s)" % len(problems) if problems else ", all hold together"))
    return 1 if problems else 0


NEW_README = """# {title} ({name})

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the {soc} family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `metal.args` | what `tools/metal-run.sh` gives this phone: its cores, its panel's rate, the threads that copy frames, the touch poll |
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/{name}_defconfig` | the common kernel, the SoC's fragment, this device's: `make {name}_defconfig` |
| `configs/{name}.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer ({soc}): `soc.conf`, `configs/{soc}.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/{name}.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check {name}`.
"""


def cmd_new(a):
    name = a.name
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", name):
        raise Problem("a device's name is its codename, lowercase: %s" % name)
    top = home("device", name)
    if os.path.exists(top):
        raise Problem("devices/%s already exists" % name)
    if a.fdt or a.adb:
        # the phone's own device tree first: tools/new-device.py makes the directory with soc.args and its README
        out = subprocess.run([sys.executable, os.path.join(HERE, "new-device.py"), name] + (["--fdt", a.fdt] if a.fdt else ["--adb"]),
                             cwd=ROOT, capture_output=True, text=True)
        print(out.stdout.rstrip())
        if out.returncode != 0:
            raise Problem("tools/new-device.py: %s" % (out.stderr.strip() or "failed"))
    if a.soc != "none":
        # the device carries its SoC's layer: a copy of one there is (another device's, synced), or a new one to fill in
        socs = soc_homes()
        os.makedirs(top, exist_ok=True)
        if a.soc in socs:
            shutil.copytree(socs[a.soc], os.path.join(top, "soc"), ignore=shutil.ignore_patterns(".git"))
            print("hakios-device: devices/%s/soc/: the %s layer, copied from %s" % (name, a.soc, os.path.relpath(socs[a.soc], ROOT)))
        else:
            os.makedirs(os.path.join(top, "soc", "configs"))
            with open(os.path.join(top, "soc", "soc.conf"), "w", encoding="utf-8") as f:
                f.write("# the %s SoC family, carried by this device's repository (docs/porting/repositories.md)\n\nsoc %s\n"
                        "\ttitle       %s\n\tvendor      ?\n\tmaintainer  %s\n" % (a.soc, a.soc, a.soc_title or a.soc, a.maintainer))
            with open(os.path.join(top, "soc", "configs", a.soc + ".config"), "w", encoding="utf-8") as f:
                f.write('# HakiOS fragment: the %s family -- what is true of the SoC, whatever device it is in.\nCONFIG_SOC="%s"\n' % (a.soc, a.soc))
            print("hakios-device: devices/%s/soc/: a new SoC layer for %s: its cores, addresses and drivers go there" % (name, a.soc))
    hc = config_tool()
    # what a new device begins without: every driver written for one machine (not QEMU's), but the ones its SoC's fragment builds
    soc_fragment = os.path.join(top, "soc", "configs", a.soc + ".config")      # the copy the device now carries
    soc_has = hc.parse(soc_fragment)[0] if a.soc != "none" else {}
    left_out = [d.symbol for d in hc.DRIVERS if not d.name.startswith("VIRTIO_") and soc_has.get(d.symbol) != "y"]
    os.makedirs(os.path.join(top, "configs"), exist_ok=True)
    with open(os.path.join(top, "device.conf"), "w", encoding="utf-8") as f:
        f.write("# devices/%s/device.conf: what this device repository is (docs/porting/repositories.md has every word).\n\n"
                "device %s\n\ttitle       %s\n\tvendor      %s\n\tsoc         %s\n\tdefconfig   %s_defconfig\n"
                "\tmaintainer  %s\n\tstatus      display     partial   UEFI's framebuffer, through the loader (to be checked)\n" %
                (name, name, a.title, a.vendor, a.soc, name, a.maintainer))
    with open(os.path.join(top, "metal.args"), "w", encoding="utf-8") as f:
        f.write("# what tools/metal-run.sh gives this phone (docs/porting/index.md): its big core and cores, its panel's rate,\n"
                "# how many threads copy frames, how often the touch screen is read. A line of boot-arguments; # is a comment.\n"
                "# fb_intf=<physical> as well, if its SoC's display interface is known: frames are then written behind the scan.\n"
                "fb_hz=60 fb_threads=3\n")
    with open(os.path.join(top, "configs", name + "_defconfig"), "w", encoding="utf-8") as f:
        f.write("# HakiOS defconfig for the %s (%s): the common kernel, %s, and this device's own.\n#include common_defconfig\n%s"
                "#include %s.config\n" % (a.title, name, "the %s family's facts" % a.soc if a.soc != "none" else "no SoC repository",
                                           "#include %s.config\n" % a.soc if a.soc != "none" else "", name))
    with open(os.path.join(top, "configs", name + ".config"), "w", encoding="utf-8") as f:
        f.write("# HakiOS fragment: the %s (%s) -- what is true of this device and of no other.\n" % (a.title, name))
        f.write("# Every phone driver is left out until it has been made to work here; the arguments only those drivers read go too.\n")
        f.write('CONFIG_DEVICE="%s"\n' % name)
        for sym in left_out:
            f.write("# CONFIG_%s is not set\n" % sym)
        common = hc.overlay({}, [hc.find_config(ROOT, "common_defconfig")] + ([soc_fragment] if a.soc != "none" else []))
        problems, _ = hc.check(dict(common, **{s: "n" for s in left_out}))
        for p in problems:
            m = re.match(r"CONFIG_([A-Z0-9_]+) gives ", p)
            if m:
                f.write("# CONFIG_%s is not set\n" % m.group(1))
        f.write('# CONFIG_FIRMWARE is "device" in the common kernel: say "none" here until this device\'s firmware is pulled\n')
    readme = os.path.join(top, "README.md")
    if os.path.isfile(readme):              # new-device.py's, about the device tree: the repository's part goes after it
        with open(readme, "a", encoding="utf-8") as f:
            f.write("\n## The repository\n\n" + NEW_README.format(title=a.title, name=name, soc=a.soc).split("\n", 2)[2])
    else:
        with open(readme, "w", encoding="utf-8") as f:
            f.write(NEW_README.format(title=a.title, name=name, soc=a.soc))
    print("hakios-device: devices/%s made: device.conf, configs/%s_defconfig, configs/%s.config, README.md" % (name, name, name))
    print("hakios-device: %d phone drivers left out to begin with; `make %s_defconfig && make config` shows what is built" % (len(left_out), name))
    print("hakios-device: next: docs/porting/index.md, step by step; tools/new-device.py reads the phone's own device tree")
    if a.repo:
        git("init", "-q", "-b", "main", cwd=top)
        git("add", "-A", cwd=top)
        git("commit", "-q", "-m", "%s: a HakiOS device repository, made by tools/hakios-device.py new" % name, cwd=top)
        print("hakios-device: devices/%s is a git repository; to publish it: make an empty repository (hakios-%s), then\n"
              "    git -C devices/%s remote add origin <its URL> && git -C devices/%s push -u origin main\n"
              "and add its block to devices/manifest.conf (repository, revision) in a pull request" % (name, name, name, name))
    return cmd_check(argparse.Namespace(names=[name]))


def git(*args, cwd=None):
    out = subprocess.run(["git", *args], cwd=cwd or ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        raise Problem("git %s: %s" % (" ".join(args), out.stderr.strip()))
    return out.stdout.strip()


def cmd_sync(a):
    entries = [(k, n, f) for k, n, f, _l in manifest() if not a.names or n in a.names]
    done = 0
    for kind, name, fields in entries:
        repository, revision = one(fields, "repository"), one(fields, "revision")
        if not repository:
            continue                        # kept in this tree (`path`)
        top = home(kind, name)
        if not os.path.isdir(os.path.join(top, ".git")):
            if os.path.exists(top):
                raise Problem("%s exists and is not a clone of %s: move it away first" % (os.path.relpath(top, ROOT), repository))
            print("hakios-device: %s: cloning %s" % (name, repository))
            git("clone", "-q", repository, top)
        else:
            # the manifest says where the repository is: a clone made before it moved or was renamed follows it
            origin = subprocess.run(["git", "remote", "get-url", "origin"], cwd=top, capture_output=True, text=True).stdout.strip()
            if origin != repository:
                print("hakios-device: %s: origin was %s, now %s (as the manifest says)" % (name, origin or "(none)", repository))
                git("remote", "set-url" if origin else "add", "origin", repository, cwd=top)
            git("fetch", "-q", "origin", cwd=top)
        if revision:
            git("checkout", "-q", revision, cwd=top)
        print("hakios-device: %s %s at %s" % (kind, name, git("rev-parse", "--short=12", "HEAD", cwd=top)))
        done += 1
    print("hakios-device: %d repositories synced%s" % (done, "" if done else " (every one the manifest names is kept in this tree)"))
    return 0


def cmd_lock(a):
    path = os.path.join(ROOT, "devices", "manifest.conf")
    lines = open(path, encoding="utf-8").read().split("\n")
    for kind, name, fields, start in manifest():
        if name != a.name:
            continue
        if not one(fields, "repository"):
            raise Problem("%s is kept in this tree: it has no revision to lock" % name)
        sha = git("rev-parse", "HEAD", cwd=home(kind, name))
        if "revision" in fields:
            n = fields["revision"][-1][1] - 1
            lines[n] = re.sub(r"revision\s+\S+", "revision    " + sha, lines[n])
        else:
            lines.insert(start, "\trevision    " + sha)
        open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
        print("hakios-device: %s locked at %s" % (name, sha[:12]))
        return 0
    raise Problem("no %s in devices/manifest.conf" % a.name)


def cmd_split(a):
    kinds = {n: k for k, n in present()}
    if a.name not in kinds:
        raise Problem("no device or SoC %s checked out" % a.name)
    prefix = os.path.relpath(home(kinds[a.name], a.name), ROOT)
    if os.path.exists(a.dir):
        raise Problem("%s already exists" % a.dir)
    branch = "split/" + a.name
    try:
        sha = git("subtree", "split", "-q", "--prefix=" + prefix, "-b", branch)
    except Problem:
        sha = None
    if sha:
        git("clone", "-q", "--branch", branch, ROOT, a.dir)
        git("branch", "-D", branch)
        git("remote", "remove", "origin", cwd=a.dir)
        how = "with the history of %s" % prefix
    else:                                   # no git subtree here: the files as they are, in one first commit
        shutil.copytree(os.path.join(ROOT, prefix), a.dir)
        git("init", "-q", cwd=a.dir)
        git("add", "-A", cwd=a.dir)
        git("commit", "-q", "-m", "%s: the device repository, split from the common one" % a.name, cwd=a.dir)
        how = "in one commit (git subtree is not installed: the history stays in the common repository)"
    print("hakios-device: %s is a repository of its own in %s, %s" % (prefix, a.dir, how))
    print("hakios-device: to publish it: make an empty repository (e.g. hakios-%s-%s), then\n"
          "    git -C %s remote add origin <its URL> && git -C %s push -u origin HEAD:main\n"
          "and in devices/manifest.conf replace `path %s` by `repository <its URL>`, then tools/hakios-device.py lock %s" %
          (kinds[a.name], a.name, a.dir, a.dir, prefix, a.name))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="the devices and SoCs").set_defaults(fn=cmd_list)
    p = sub.add_parser("check", help="a device or SoC repository holds together")
    p.add_argument("names", nargs="*")
    p.set_defaults(fn=cmd_check)
    p = sub.add_parser("new", help="a new device repository")
    p.add_argument("name")
    p.add_argument("--soc", required=True, help="its SoC family: copied from a device or socs/ that has it, else a new layer; or none")
    p.add_argument("--soc-title", help="for a new SoC layer: what the family is called")
    p.add_argument("--repo", action="store_true", help="make the device's directory a git repository, ready to push")
    p.add_argument("--title", required=True, help='what it is called ("POCO X5 Pro 5G")')
    p.add_argument("--vendor", default="?")
    p.add_argument("--maintainer", default="?")
    tree = p.add_mutually_exclusive_group()
    tree.add_argument("--fdt", help="the phone's own device tree (adb pull /sys/firmware/fdt): tools/new-device.py reads it first")
    tree.add_argument("--adb", action="store_true", help="... pulled from the phone on adb now")
    p.set_defaults(fn=cmd_new)
    p = sub.add_parser("sync", help="clone or update the repositories the manifest names")
    p.add_argument("names", nargs="*")
    p.set_defaults(fn=cmd_sync)
    p = sub.add_parser("lock", help="write the revision checked out into the manifest")
    p.add_argument("name")
    p.set_defaults(fn=cmd_lock)
    p = sub.add_parser("split", help="a device or SoC kept in this tree as a repository of its own")
    p.add_argument("name")
    p.add_argument("dir")
    p.set_defaults(fn=cmd_split)
    a = ap.parse_args()
    try:
        sys.exit(a.fn(a))
    except Problem as e:
        print("hakios-device: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
