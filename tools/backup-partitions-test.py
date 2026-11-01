#!/usr/bin/env python3
"""tools/backup-partitions.py against a stand-in adb: a "phone" that is a folder of files.

What is checked: the dry run copies nothing; a real run copies exactly the bytes, proves them with three hashes and writes the
manifest and the restore notes (which are text, never run); a partition the phone lacks is skipped and said; a partition that
changes while it is read is read again, and one that never holds still is not kept; a phone without root stops the tool before
a byte is copied; failed reads and checks leave no temporary files or unverified images and preserve previous copies;
and the stand-in saw only reads (no `of=`, no write).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "backup-partitions.py")
failures = []

# the stand-in adb: `adb exec-out su -c "<command>"` on a phone that is the folder $FAKE_PHONE (parts/NAME, disk_sda)
FAKE = r'''
import hashlib, os, re, sys
root = os.environ["FAKE_PHONE"]
mode = os.environ.get("FAKE_MODE", "")
args = sys.argv[1:]
log = open(os.path.join(root, "commands.log"), "a")
if args[:3] != ["exec-out", "su", "-c"]:
    sys.exit(2)
cmd = args[3]
log.write(cmd + "\n"); log.close()
if mode == "noroot":
    sys.stderr.write("su: not allowed\n"); sys.exit(1)
names = sorted(os.listdir(os.path.join(root, "parts")))
node_file = {"/dev/block/sda%d" % (i + 1): os.path.join(root, "parts", n) for i, n in enumerate(names)}
node_file["/dev/block/sda"] = os.path.join(root, "disk_sda")
counter = os.path.join(root, "hashcount")

def data(node, skip=None, count=None, bs=1048576):
    b = open(node_file[node], "rb").read()
    if skip is not None:
        b = b[skip * bs: (skip + count) * bs]
    return b

def sha(b):
    return hashlib.sha256(b).hexdigest()

m = re.fullmatch(r"ls -l /dev/block/by-name", cmd)
if m:
    for i, n in enumerate(names):
        print("lrwxrwxrwx 1 root root 16 2024-01-01 00:00 %s -> /dev/block/sda%d" % (n, i + 1))
    sys.exit(0)
m = re.fullmatch(r"blockdev --getsize64 (\S+)", cmd)
if m:
    print(os.path.getsize(node_file[m.group(1)])); sys.exit(0)
m = re.fullmatch(r"sha256sum (\S+)", cmd)
if m:
    b = data(m.group(1))
    if mode in ("hash-before", "hash-after"):
        n = int(open(counter).read()) if os.path.exists(counter) else 0
        open(counter, "w").write(str(n + 1))
        if mode == "hash-before" or n > 0:
            sys.stderr.write("sha256sum: read error\n"); sys.exit(1)
    if mode == "change":                       # the partition changes under the copy: the first hash is of the old content
        n = int(open(counter).read()) if os.path.exists(counter) else 0
        open(counter, "w").write(str(n + 1))
        if n == 0:                             # only the first look: the second attempt finds it holding still
            with open(node_file[m.group(1)], "r+b") as f:
                f.seek(0); f.write(bytes([(b[0] + 1) % 256]))
    print("%s  %s" % (sha(b), m.group(1))); sys.exit(0)
m = re.fullmatch(r"dd if=(\S+) bs=(\d+) skip=(\d+) count=(\d+) 2>/dev/null \| sha256sum", cmd)
if m:
    print("%s  -" % sha(data(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(2))))); sys.exit(0)
m = re.fullmatch(r"dd if=(\S+) bs=(\d+) skip=(\d+) count=(\d+) 2>/dev/null", cmd)
if m:
    sys.stdout.buffer.write(data(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(2)))); sys.exit(0)
m = re.fullmatch(r"dd if=(\S+) bs=1048576 2>/dev/null", cmd)
if m:
    b = data(m.group(1))
    if mode == "short":
        sys.stdout.buffer.write(b[:-1]); sys.exit(0)
    if mode == "readfail":
        sys.stdout.buffer.write(b[:len(b) // 2])
        sys.stderr.write("device offline\n"); sys.exit(1)
    sys.stdout.buffer.write(b)
    if mode == "never":                         # one that never holds still: every look is different
        with open(node_file[m.group(1)], "r+b") as f:
            f.seek(0); f.write(bytes([(b[0] + 1) % 256]))
    sys.exit(0)
sys.stderr.write("stand-in adb: no such command: %s\n" % cmd); sys.exit(3)
'''


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:500])


def phone(root, parts):
    os.makedirs(os.path.join(root, "parts"))
    for name, content in parts.items():
        open(os.path.join(root, "parts", name), "wb").write(content)
    open(os.path.join(root, "disk_sda"), "wb").write(bytes(range(256)) * 16 * 64)       # 64 blocks of 4096
    open(os.path.join(root, "fake_adb.py"), "w").write(FAKE)


def run(root, out, *flags, mode=""):
    env = dict(os.environ, FAKE_PHONE=root, FAKE_MODE=mode)
    return subprocess.run([sys.executable, TOOL, out, "--adb", sys.executable, "--adb-arg", os.path.join(root, "fake_adb.py")] + list(flags),
                          capture_output=True, text=True, env=env)


work = tempfile.mkdtemp(prefix="backup-test-")
try:
    boot = os.urandom(3000)
    persist = os.urandom(70000)
    parts = {"boot_a": boot, "persist": persist, "userdata": os.urandom(200000), "vbmeta_a": b"AVB0" + os.urandom(100)}
    root = os.path.join(work, "phone")
    phone(root, parts)

    out = os.path.join(work, "out-dry")
    r = run(root, out)
    check("dry run: says what it would read, copies nothing", r.returncode == 0 and not os.path.exists(out) and "boot_a" in r.stdout and "nothing was copied" in r.stdout, r.stdout + r.stderr)
    check("dry run: the partitions the phone lacks are said, not invented", "boot_b" in r.stdout and "the phone does not have it" in r.stdout, r.stdout)
    check("dry run: userdata is never in the plan", "userdata" not in r.stdout, r.stdout)

    out = os.path.join(work, "out")
    r = run(root, out, "--yes")
    check("a real run succeeds", r.returncode == 0, r.stdout + r.stderr)
    same = all(open(os.path.join(out, n + ".img"), "rb").read() == parts[n] for n in ("boot_a", "persist", "vbmeta_a"))
    check("the copies are the bytes of the partitions", same)
    check("userdata was not copied", not os.path.exists(os.path.join(out, "userdata.img")))
    manifest = json.load(open(os.path.join(out, "manifest.json")))
    by = {p["name"]: p for p in manifest["partitions"]}
    check("the manifest has each hash", by["boot_a"]["sha256"] == hashlib.sha256(boot).hexdigest() and by["persist"]["bytes"] == len(persist), by)
    disk = open(os.path.join(root, "disk_sda"), "rb").read()
    heads = [t for t in manifest["tables"] if t["part"] == "head"]
    check("the partition tables: the head and the tail of the disk, 8 blocks each",
          len(manifest["tables"]) == 2 and heads and open(os.path.join(out, "gpt-sda-head.bin"), "rb").read() == disk[:8 * 4096]
          and open(os.path.join(out, "gpt-sda-tail.bin"), "rb").read() == disk[-8 * 4096:], manifest["tables"])
    restore = open(os.path.join(out, "restore.txt")).read()
    check("restore.txt names the fastboot lines and is only text", "fastboot flash persist persist.img" in restore and restore.startswith("# What would put these back"), restore)
    log = open(os.path.join(root, "commands.log")).read()
    check("the stand-in saw only reads: no of=, no write, no flash", "of=" not in log and "write" not in log and "flash" not in log and ">" not in log.replace("2>/dev/null", ""), log[:400])

    # a partition that changes while it is read is read again and kept once it holds still
    shutil.rmtree(root); phone(root, parts)
    out = os.path.join(work, "out-change")
    r = run(root, out, "--yes", "--only", "boot_a", mode="change")
    check("a partition that changes under the copy is read again", r.returncode == 0 and "the hashes differ" in r.stdout, r.stdout + r.stderr)
    check("... and what was kept matches the partition as it is now", open(os.path.join(out, "boot_a.img"), "rb").read() == open(os.path.join(root, "parts", "boot_a"), "rb").read())

    # one that never holds still is not kept
    shutil.rmtree(root); phone(root, parts)
    out = os.path.join(work, "out-never")
    r = run(root, out, "--yes", "--only", "boot_a", mode="never")
    check("a partition that never holds still stops the tool, with the reason", r.returncode == 1 and "could not be proven" in r.stderr, r.stdout + r.stderr)
    check("... and its copy is not left behind", not os.path.exists(os.path.join(out, "boot_a.img")))

    # no root: stopped before a byte is copied
    shutil.rmtree(root); phone(root, parts)
    out = os.path.join(work, "out-noroot")
    r = run(root, out, "--yes", mode="noroot")
    check("a phone without root stops the tool before anything is copied", r.returncode == 1 and "STOPPED" in r.stderr and not os.path.exists(out), r.stdout + r.stderr)

    for mode, reason in (("short", "bytes arrived"), ("readfail", "device offline"),
                         ("hash-before", "sha256sum: read error"), ("hash-after", "sha256sum: read error"),
                         ("never", "could not be proven")):
        for existing in (False, True):
            shutil.rmtree(root); phone(root, parts)
            out = os.path.join(work, "out-%s-%s" % (mode, "existing" if existing else "new"))
            os.makedirs(out)
            previous = {"boot_a.img": b"previous proven copy", "manifest.json": b"previous manifest", "restore.txt": b"previous restore notes"}
            if existing:
                for name, content in previous.items():
                    open(os.path.join(out, name), "wb").write(content)
            r = run(root, out, "--yes", "--only", "boot_a", mode=mode)
            label = "%s (%s destination)" % (mode, "existing" if existing else "new")
            check(label + ": stopped with the reason", r.returncode == 1 and reason in r.stderr and "Traceback" not in r.stderr, r.stdout + r.stderr)
            check(label + ": no unverified or temporary files", sorted(os.listdir(out)) == (sorted(previous) if existing else []), os.listdir(out))
            if existing:
                check(label + ": previous backup is unchanged", all(os.path.isfile(os.path.join(out, name)) and open(os.path.join(out, name), "rb").read() == content for name, content in previous.items()))

    # A new, proven copy may replace an old one, with no staging file left behind.
    shutil.rmtree(root); phone(root, parts)
    out = os.path.join(work, "out-replace")
    os.makedirs(out)
    open(os.path.join(out, "boot_a.img"), "wb").write(b"old backup")
    r = run(root, out, "--yes", "--only", "boot_a")
    check("a proven replacement succeeds", r.returncode == 0, r.stdout + r.stderr)
    check("the replacement has the current bytes", open(os.path.join(out, "boot_a.img"), "rb").read() == boot)
    check("a successful replacement leaves only final files", sorted(os.listdir(out)) == ["boot_a.img", "manifest.json", "restore.txt"], os.listdir(out))
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("backup-partitions-test: %d FAILED" % len(failures))
print("backup-partitions-test: all passed")
