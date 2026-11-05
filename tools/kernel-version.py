#!/usr/bin/env python3
"""The version string of the HakiOS kernel, made from VERSION, the active configuration and git.

usage: tools/kernel-version.py [--short | --name | --darwin | --release | --all]
                               [--config FILE] [--root DIR]

  (none)    2.3.4-hakios-darwin22.5-sc7280-g1a2b3c4d[-dirty]   what goes under the banner as kernel.ver=
  --short   2.3.4                                              VERSION.PATCHLEVEL.SUBLEVEL
  --release 2.3.4-hakios                                       with EXTRAVERSION: what a tag is called (v2.3.4-hakios)
  --name    Hello Darwin                                        the release's name
  --darwin  22.5.0 xnu-8796.121.2                               what it is built on
  --all     every part, one per line, for the build log

The parts of the full string, in the order Android's kernels give theirs:
  VERSION.PATCHLEVEL.SUBLEVEL  from the file VERSION
  EXTRAVERSION                 ... likewise
  -darwin<major>.<minor>       the Darwin it is built on (DARWIN in VERSION)
  -<soc>                       CONFIG_SOC of the active configuration, if there is one (`make redwood_defconfig`)
  -g<commit>[-dirty]           git: the commit, and "-dirty" if a tracked file differs from it

HAKIOS_VERSION_SUFFIX in the environment replaces the git part (a reproducible build, or a tree that is
not a git checkout): the string it holds is added as it is.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read_version(root):
    """VERSION as {KEY: value}: `KEY = value` lines, `#` comments."""
    path = os.path.join(root, "VERSION")
    if not os.path.isfile(path):
        sys.exit("kernel-version: no VERSION file in %s" % root)
    values = {}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"([A-Z]+)\s*=\s*(.*)", line)
        if not m:
            sys.exit("kernel-version: VERSION:%d: want  KEY = value, got %r" % (n, line))
        values[m.group(1)] = m.group(2).strip()
    for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL", "EXTRAVERSION", "NAME", "DARWIN", "XNU"):
        if key not in values:
            sys.exit("kernel-version: VERSION lacks %s" % key)
    for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL"):
        if not values[key].isdigit():
            sys.exit("kernel-version: %s must be a number, got %r" % (key, values[key]))
    if values["EXTRAVERSION"] and not re.fullmatch(r"-[A-Za-z0-9._]+", values["EXTRAVERSION"]):
        sys.exit("kernel-version: EXTRAVERSION is a dash and letters or digits (-hakios), got %r" % values["EXTRAVERSION"])
    return values


def config_soc(path):
    """CONFIG_SOC of a .config, or None: the SoC family the configuration is for."""
    if not path or not os.path.isfile(path):
        return None
    for line in open(path, encoding="utf-8"):
        m = re.fullmatch(r'CONFIG_SOC="?([a-z0-9]+)"?\s*', line.strip())
        if m:
            return m.group(1)
    return None


def git_part(root):
    """-g<commit>[-dirty], or "" when this is not a git tree (or git is not there)."""
    def git(*args):
        return subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)

    try:
        head = git("rev-parse", "--short=8", "HEAD")
    except OSError:
        return ""
    if head.returncode != 0:
        return ""
    git("update-index", "-q", "--refresh")          # stat data may be stale after a checkout: not a change
    dirty = git("diff-index", "--quiet", "HEAD", "--").returncode != 0
    return "-g" + head.stdout.strip() + ("-dirty" if dirty else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    what = ap.add_mutually_exclusive_group()
    for flag in ("short", "release", "name", "darwin", "all"):
        what.add_argument("--" + flag, action="store_true")
    ap.add_argument("--config", help="the configuration to take CONFIG_SOC from (default: .config at the top)")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    v = read_version(root)
    short = "%s.%s.%s" % (v["VERSION"], v["PATCHLEVEL"], v["SUBLEVEL"])
    release = short + v["EXTRAVERSION"]
    darwin = "-darwin" + ".".join(v["DARWIN"].split(".")[:2])
    soc = config_soc(args.config or os.path.join(root, ".config"))
    suffix = os.environ.get("HAKIOS_VERSION_SUFFIX")
    git = suffix if suffix is not None else git_part(root)
    full = release + darwin + ("-" + soc if soc else "") + git

    if args.short:
        print(short)
    elif args.release:
        print(release)
    elif args.name:
        print(v["NAME"])
    elif args.darwin:
        print(v["DARWIN"], v["XNU"])
    elif args.all:
        print("full=" + full)
        print("short=" + short)
        print("release=" + release)
        print("name=" + v["NAME"])
        print("darwin=" + v["DARWIN"])
        print("xnu=" + v["XNU"])
        print("soc=" + (soc or ""))
        print("git=" + git)
    else:
        print(full)


if __name__ == "__main__":
    main()
