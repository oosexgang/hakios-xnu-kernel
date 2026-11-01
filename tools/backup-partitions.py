#!/usr/bin/env python3
"""Copy the partitions that cannot be made again off the phone, and prove the copies: READ ONLY.

usage: tools/backup-partitions.py OUTDIR [--yes] [--only NAME,NAME] [--with-bootchain] [--adb PROGRAM [--adb-arg ARG ...]]

Rule 1 of docs/install.md: before anything is ever written to a phone, what could not be made again is copied off it and
checked. This is that copy. It never writes to the phone: the only things it runs there are `ls`, `blockdev`, `dd if=`
(a read) and `sha256sum`. Without --yes it makes no copy at all and says what it would read and how much room that needs.

What is copied (the names are the phone's own, from /dev/block/by-name; one that the phone does not have is skipped and said):
  the small partitions that decide whether it boots   boot_a boot_b init_boot_a init_boot_b vendor_boot_a vendor_boot_b dtbo_a dtbo_b
                                                       vbmeta_a vbmeta_b vbmeta_system_a vbmeta_system_b misc devinfo
  the ones that hold THIS phone's identity and radio  persist modemst1 modemst2 fsg (the calibration, the IMEI, the Wi-Fi and
                                                       Bluetooth addresses: Android cannot make them again)
  the partition tables                                the first and last 8 blocks of each disk (sda ... sdf), which is what the carve
                                                       (tools/userdata-carve.py) changes
  with --with-bootchain: xbl xbl_config abl aop aop_config tz hyp devcfg qupfw uefisecapp shrm cpucp keymaster featenabler imagefv
                                                       (small; what the phone starts from; useful if it ever has to be revived)
Never copied: userdata, super and the other big ones (they are Android's, and their size is the problem).

How a copy is proven. The partition is read over adb (exec-out, so the bytes are not changed on the way), its SHA-256 is made as
it arrives, and the phone is then asked for the SHA-256 of the same partition, once BEFORE and once AFTER the copy: the three must
be the same, or the partition changed while it was read (then it is read again, up to three times) or the copy is wrong. The copy
is read into a temporary file beside its destination and moved into place only if they agree. A failed read or check removes
the temporary file and leaves any previous copy in place. manifest.json says what was copied, its size and hash, and when;
restoring is NOT done by this tool:
`restore.txt` says which fastboot command would put each back, for a person to run.

These files are the phone's own and some of them (persist, modemst*, fsg) hold its identity: keep them off the network, and do
not commit them (the folder name backups/ is ignored by git).

Needs root on the phone (`su`): the partitions are not readable by the shell user.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

BOOT = ["boot_a", "boot_b", "init_boot_a", "init_boot_b", "vendor_boot_a", "vendor_boot_b", "dtbo_a", "dtbo_b", "vbmeta_a", "vbmeta_b",
        "vbmeta_system_a", "vbmeta_system_b", "misc", "devinfo"]
IDENTITY = ["persist", "modemst1", "modemst2", "fsg"]
BOOTCHAIN = ["xbl", "xbl_config", "abl", "aop", "aop_config", "tz", "hyp", "devcfg", "qupfw", "uefisecapp", "shrm", "cpucp", "keymaster",
             "featenabler", "imagefv"]
BLOCK = 4096
TRIES = 3
ENOUGH_FREE = 1.5           # the room that has to be free on the host, as a multiple of what is copied


class Failure(Exception):
    pass


def device(adb, command, binary=False):
    """Run `command` as root on the phone; its output, or a Failure."""
    r = subprocess.run(adb + ["exec-out", "su", "-c", command], capture_output=True)
    if r.returncode != 0:
        raise Failure("on the phone `%s` failed (%d): %s" % (command, r.returncode, r.stderr.decode(errors="replace").strip()[:200]))
    return r.stdout if binary else r.stdout.decode(errors="replace")


def by_name(adb):
    """{partition name: block device} from /dev/block/by-name, resolved to the device the link points at."""
    out = device(adb, "ls -l /dev/block/by-name")
    parts = {}
    for line in out.splitlines():
        m = re.search(r"\s(\S+) -> (/dev/block/\S+)\s*$", line.strip())
        if m:
            parts[m.group(1)] = m.group(2)
    if not parts:
        raise Failure("the phone shows no partitions under /dev/block/by-name (is the shell root? `adb shell su -c id`)")
    return parts


def size_of(adb, node):
    out = device(adb, "blockdev --getsize64 %s" % node).strip()
    if not out.isdigit():
        raise Failure("%s: the size is %r" % (node, out))
    return int(out)


def hash_on_phone(adb, node, count=None, skip=None):
    """SHA-256 of the whole device, or of `count` 4 KB blocks after `skip` of them."""
    if count is None:
        cmd = "sha256sum %s" % node
    else:
        cmd = "dd if=%s bs=%d skip=%d count=%d 2>/dev/null | sha256sum" % (node, BLOCK, skip, count)
    out = device(adb, cmd).split()
    if not out or not re.fullmatch(r"[0-9a-f]{64}", out[0]):
        raise Failure("%s: the phone's sha256sum said %r" % (node, out[:1]))
    return out[0]


def copy_one(adb, node, path, size, skip=None, count=None):
    """Read the device over adb into `path`; the SHA-256 of what arrived."""
    if count is None:
        cmd = "dd if=%s bs=1048576 2>/dev/null" % node
    else:
        cmd = "dd if=%s bs=%d skip=%d count=%d 2>/dev/null" % (node, BLOCK, skip, count)
    h = hashlib.sha256()
    total = 0
    with subprocess.Popen(adb + ["exec-out", "su", "-c", cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p, open(path, "wb") as out:
        for chunk in iter(lambda: p.stdout.read(1 << 20), b""):
            out.write(chunk)
            h.update(chunk)
            total += len(chunk)
        p.wait()
        err = p.stderr.read().decode(errors="replace").strip()
    if p.returncode != 0:
        raise Failure("reading %s failed (%d): %s" % (node, p.returncode, err[:200]))
    if total != size:
        raise Failure("%s: %d bytes arrived, %d were expected" % (node, total, size))
    return h.hexdigest()


def proven_copy(adb, node, path, size, skip=None, count=None):
    """A copy that three hashes agree on (the phone's before, the host's, the phone's after), or a Failure."""
    fd, pending = tempfile.mkstemp(prefix="." + os.path.basename(path) + ".", suffix=".part",
                                  dir=os.path.dirname(os.path.abspath(path)))
    os.close(fd)
    try:
        last = ""
        for attempt in range(1, TRIES + 1):
            before = hash_on_phone(adb, node, count, skip)
            mine = copy_one(adb, node, pending, size, skip, count)
            after = hash_on_phone(adb, node, count, skip)
            if before == mine == after:
                os.replace(pending, path)
                return mine
            last = "phone before %s.., host %s.., phone after %s.." % (before[:12], mine[:12], after[:12])
            print("    attempt %d: the hashes differ (%s)" % (attempt, last))
        raise Failure("%s: the copy could not be proven after %d tries (%s); it was not kept" % (node, TRIES, last))
    finally:
        try:
            os.remove(pending)
        except FileNotFoundError:
            pass


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--yes", action="store_true", help="really copy (without it: say what would be)")
    ap.add_argument("--only", help="only these partitions, comma separated (and their disks' tables are not copied)")
    ap.add_argument("--with-bootchain", action="store_true")
    ap.add_argument("--adb", default="adb", help="the adb program (tests use a stand-in)")
    ap.add_argument("--adb-arg", action="append", default=[], help="an argument to put after the adb program (tests: the stand-in's script)")
    a = ap.parse_args(argv)
    adb = [a.adb] + a.adb_arg
    try:
        parts = by_name(adb)
        wanted = [n.strip() for n in a.only.split(",")] if a.only else BOOT + IDENTITY + (BOOTCHAIN if a.with_bootchain else [])
        plan, missing = [], []
        for name in wanted:
            if name in parts:
                plan.append((name, parts[name], size_of(adb, parts[name])))
            else:
                missing.append(name)
        disks = {}
        if not a.only:
            for node in sorted({re.sub(r"p?\d+$", "", n) for n in parts.values() if re.search(r"/sd[a-z]\d+$", n)}):
                disks[node] = size_of(adb, node)
        total = sum(s for _n, _d, s in plan) + sum(2 * 8 * BLOCK for _ in disks)
        print("backup-partitions: %d partitions to copy, %d disks' tables, %.1f MB in all" % (len(plan), len(disks), total / 1e6))
        for name, node, size in plan:
            print("    %-18s %-26s %10d bytes" % (name, node, size))
        for node in disks:
            print("    %-18s %-26s first and last 8 blocks" % ("table of " + os.path.basename(node), node))
        for name in missing:
            print("    %-18s the phone does not have it: skipped" % name)
        if not a.yes:
            print("backup-partitions: nothing was copied (add --yes). This tool only reads from the phone.")
            return 0
        os.makedirs(a.outdir, exist_ok=True)
        free = __import__("shutil").disk_usage(a.outdir).free
        if free < total * ENOUGH_FREE:
            raise Failure("%.1f MB free in %s, and %.1f MB is %.1f times what is copied: not enough" % (free / 1e6, a.outdir, total * ENOUGH_FREE / 1e6, ENOUGH_FREE))
        manifest = {"made": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                    "note": "read only; the hashes were checked on the phone before and after each copy", "partitions": [], "tables": []}
        restore = ["# What would put these back. NOT run by backup-partitions.py: a person runs a line, knowing why.",
                   "# Never write a bootchain partition (xbl abl tz hyp ...) unless you are certain; a wrong one cannot be undone without EDL.", ""]
        for name, node, size in plan:
            path = os.path.join(a.outdir, name + ".img")
            print("backup-partitions: %s ..." % name, flush=True)
            digest = proven_copy(adb, node, path, size)
            manifest["partitions"].append({"name": name, "node": node, "bytes": size, "sha256": digest, "file": name + ".img"})
            restore.append("fastboot flash %s %s     # sha256 %s" % (name, name + ".img", digest))
        for node, size in disks.items():
            blocks = size // BLOCK
            for label, skip in (("head", 0), ("tail", blocks - 8)):
                path = os.path.join(a.outdir, "gpt-%s-%s.bin" % (os.path.basename(node), label))
                digest = proven_copy(adb, node, path, 8 * BLOCK, skip, 8)
                manifest["tables"].append({"disk": node, "part": label, "skip_blocks": skip, "blocks": 8, "sha256": digest, "file": os.path.basename(path)})
        with open(os.path.join(a.outdir, "manifest.json"), "w", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        with open(os.path.join(a.outdir, "restore.txt"), "w", newline="\n") as f:
            f.write("\n".join(restore) + "\n")
        print("backup-partitions: %d partitions and %d table copies proven and kept in %s (manifest.json, restore.txt)" %
              (len(manifest["partitions"]), len(manifest["tables"]), a.outdir))
        return 0
    except Failure as e:
        print("backup-partitions: STOPPED: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
