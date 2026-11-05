#!/usr/bin/env python3
"""tools/kernel-version.py: how the version string is made, and what it refuses.

  the real VERSION file          -> 2.3.4-hakios-darwin22.5-g<8 hex>, in the form Android's kernels have
  a tracked file changed         -> "-dirty"; a new untracked file is not a change (as in Linux's setlocalversion)
  CONFIG_SOC in a .config        -> "-<soc>" between the Darwin and the commit
  HAKIOS_VERSION_SUFFIX          -> replaces the git part
  not a git tree                 -> no git part, and no failure
  a malformed VERSION            -> an error that says which line
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "kernel-version.py")
failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + detail.strip().replace("\n", "\n     ")[:400])


def run(root, *args, env=None):
    e = dict(os.environ)
    e.pop("HAKIOS_VERSION_SUFFIX", None)
    e.update(env or {})
    return subprocess.run([sys.executable, TOOL, "--root", root, *args], capture_output=True, text=True, env=e)


def git(d, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-C", d, *args], check=True, capture_output=True)


work = tempfile.mkdtemp(prefix="kernel-version-test-")
try:
    # a repository with the real VERSION file, committed
    repo = os.path.join(work, "repo")
    os.makedirs(repo)
    subprocess.run(["git", "init", "-q", repo], check=True)
    shutil.copyfile(os.path.join(ROOT, "VERSION"), os.path.join(repo, "VERSION"))
    open(os.path.join(repo, "tracked.txt"), "w").write("one\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "x")

    full = run(repo)
    check("the full string has the Android form", re.fullmatch(r"\d+\.\d+\.\d+-hakios-darwin22\.5-g[0-9a-f]{8}", full.stdout.strip()) is not None,
          full.stdout + full.stderr)
    check("--short is VERSION.PATCHLEVEL.SUBLEVEL", re.fullmatch(r"\d+\.\d+\.\d+", run(repo, "--short").stdout.strip()) is not None)
    check("--release adds EXTRAVERSION", run(repo, "--release").stdout.strip() == run(repo, "--short").stdout.strip() + "-hakios")
    check("--name and --darwin", run(repo, "--name").stdout.strip() == "Hello Darwin" and
          run(repo, "--darwin").stdout.split() == ["22.5.0", "xnu-8796.121.2"])
    check("the string starts with the release", full.stdout.startswith(run(repo, "--release").stdout.strip() + "-darwin"))

    open(os.path.join(repo, "untracked.txt"), "w").write("new\n")
    check("an untracked file is not a change", "-dirty" not in run(repo).stdout)
    open(os.path.join(repo, "tracked.txt"), "w").write("two\n")
    check("a changed tracked file makes it -dirty", run(repo).stdout.strip().endswith("-dirty"), run(repo).stdout)
    git(repo, "checkout", "--", "tracked.txt")
    check("restored: clean again", "-dirty" not in run(repo).stdout)

    open(os.path.join(repo, ".config"), "w").write('# a configuration\nCONFIG_SOC="sc7280"\nCONFIG_CPUS=8\n')
    check("CONFIG_SOC of .config goes between the Darwin and the commit",
          re.fullmatch(r"\d+\.\d+\.\d+-hakios-darwin22\.5-sc7280-g[0-9a-f]{8}", run(repo).stdout.strip()) is not None, run(repo).stdout)
    other = os.path.join(work, "other.config")
    open(other, "w").write('CONFIG_SOC="sm8550"\n')
    check("--config names another configuration", "-sm8550-" in run(repo, "--config", other).stdout, run(repo, "--config", other).stdout)

    check("HAKIOS_VERSION_SUFFIX replaces the git part",
          run(repo, env={"HAKIOS_VERSION_SUFFIX": "-gdeadbeef"}).stdout.strip().endswith("-sc7280-gdeadbeef"))
    check("an empty HAKIOS_VERSION_SUFFIX means no git part", run(repo, env={"HAKIOS_VERSION_SUFFIX": ""}).stdout.strip().endswith("-sc7280"))

    # not a git tree
    plain = os.path.join(work, "plain")
    os.makedirs(plain)
    shutil.copyfile(os.path.join(ROOT, "VERSION"), os.path.join(plain, "VERSION"))
    r = run(plain)
    check("not a git tree: no git part, no failure", r.returncode == 0 and r.stdout.strip() == "%s-darwin22.5" % run(plain, "--release").stdout.strip(),
          r.stdout + r.stderr)

    # malformed files
    def with_version(text):
        d = os.path.join(work, "bad%d" % len(os.listdir(work)))
        os.makedirs(d)
        open(os.path.join(d, "VERSION"), "w").write(text)
        return run(d)

    good = open(os.path.join(ROOT, "VERSION")).read()
    r = with_version(re.sub(r"SUBLEVEL = \d+", "SUBLEVEL = thirteen", good))
    check("a SUBLEVEL that is not a number: refused", r.returncode != 0 and "SUBLEVEL" in r.stderr, r.stderr)
    r = with_version(good.replace("EXTRAVERSION = -hakios", "EXTRAVERSION = hakios"))
    check("an EXTRAVERSION without its dash: refused", r.returncode != 0 and "EXTRAVERSION" in r.stderr, r.stderr)
    r = with_version(good.replace("NAME = Hello Darwin\n", ""))
    check("a missing key: refused, and named", r.returncode != 0 and "NAME" in r.stderr, r.stderr)
    r = with_version(good + "this is not a setting\n")
    check("a line that is no setting: refused, with its number", r.returncode != 0 and re.search(r"VERSION:\d+", r.stderr), r.stderr)
    r = run(os.path.join(work, "nothing-here"))
    check("no VERSION file: refused", r.returncode != 0 and "no VERSION file" in r.stderr, r.stderr)
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("kernel-version-test: %d FAILED" % len(failures))
print("kernel-version-test: all passed")
