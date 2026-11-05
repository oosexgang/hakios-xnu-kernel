#!/usr/bin/env python3
"""Read what var_store kept of Darwin's /private/var, from the phone's darwin partition, and write nothing.

usage:
  var-store-read.py show                       both slots' headers: generation, entries, bytes, when
  var-store-read.py list [PREFIX]              the newest good archive's entries (under PREFIX, a path in /private/var)
  var-store-read.py extract PREFIX DEST        the files under PREFIX into DEST (directories and files only)
  var-store-read.py selftest                   an archive made here in var_store's format, read back

The store is userland/src/var_store.c's: two slots of 2 GiB from darwin's byte 8589934592, each a header block
(struct head) and then a stream of entries (struct entry, its path, its extended attributes, its contents, padded
to 8 bytes), whose CRC-32 is in the header. The newest slot whose header and stream both check out is read; one
that does not check out is never half used.

Where it reads from, read only, either way:
  --image FILE   a copy of darwin from byte 8589934592 (dd if=/dev/block/by-name/darwin bs=4096 skip=2097152)
  (default)      the phone in Android, over adb, as root: `su -c dd` of /dev/block/by-name/darwin, the blocks
                 needed and no others. Under Git Bash on Windows as anywhere else (adb exec-out passes bytes as
                 they are).

What it is for: tools/metal-cache-seed.sh takes db/hakios-metal-cache (Metal's compiled shader stages) out of
the last run's store, so that the next image carries them and no boot compiles them again.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

BLOCK = 4096
STORE_AT = 8589934592
SLOT_BYTES = 0x80000000
SLOTS = 2
HEAD = struct.Struct("<8sIIQQQQII")           # magic version slot generation bytes entries seconds crc head_crc
ENTRY = struct.Struct("<IIIIIIIIqQ")          # kind mode uid gid flags pathlen nxattr mtime_nsec mtime_sec size
MAGIC = b"RDWVAR\0\0"
ENT_DIR, ENT_FILE, ENT_SYMLINK, ENT_HARDLINK, ENT_DIRTIME = 1, 2, 3, 4, 5
DEVICE = "/dev/block/by-name/darwin"


class BadArchive(ValueError):
    """A slot's bytes do not agree with its otherwise valid header."""


class Source:
    """Bytes of darwin counted from STORE_AT."""

    def __init__(self, image=None):
        self.image = image

    def read(self, offset, length):
        if self.image is not None:
            with open(self.image, "rb") as f:
                f.seek(offset)
                return f.read(length)
        first = offset // BLOCK
        count = (offset + length + BLOCK - 1) // BLOCK - first
        skip = STORE_AT // BLOCK + first
        # (read only: if= the partition by name, no of=; the device is darwin's or dd fails)
        cmd = ["adb", "exec-out", "su", "-c", f"dd if={DEVICE} bs={BLOCK} skip={skip} count={count} 2>/dev/null"]
        data = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
        if len(data) != count * BLOCK:
            raise SystemExit(f"var-store-read: adb gave {len(data)} bytes where {count * BLOCK} were asked for "
                             "(is the phone in Android, with root for the shell?)")
        start = offset - first * BLOCK
        return data[start:start + length]


def head_crc(raw):
    zeroed = raw[:HEAD.size - 4] + b"\0\0\0\0"
    return zlib.crc32(zeroed) & 0xffffffff


def read_head(src, slot):
    raw = src.read(slot * SLOT_BYTES, HEAD.size)
    if len(raw) < HEAD.size:
        return None
    magic, version, s, generation, nbytes, entries, seconds, crc, hcrc = HEAD.unpack(raw)
    if magic != MAGIC or version != 1 or s != slot or nbytes > SLOT_BYTES - BLOCK or hcrc != head_crc(raw):
        return None
    return dict(slot=slot, generation=generation, bytes=nbytes, entries=entries, seconds=seconds, crc=crc)


def newest_archive(src):
    """The newest header and verified stream, falling back past damaged slots."""
    heads = []
    for slot in range(SLOTS):
        h = read_head(src, slot)
        if h is not None:
            heads.append(h)
    for h in sorted(heads, key=lambda h: h["generation"], reverse=True):
        try:
            return h, stream_of(src, h)
        except BadArchive as error:
            print(error, file=sys.stderr)
    return None, None


def stream_of(src, h):
    data = src.read(h["slot"] * SLOT_BYTES + BLOCK, h["bytes"])
    if len(data) != h["bytes"] or (zlib.crc32(data) & 0xffffffff) != h["crc"]:
        raise BadArchive(f"var-store-read: slot {h['slot']}: the archive's checksum is not its header's: not read")
    return data


def entries(data):
    at = 0
    while at < len(data):
        if len(data) - at < ENTRY.size:
            raise SystemExit("var-store-read: an entry cut short")
        kind, mode, uid, gid, flags, pathlen, nxattr, nsec, sec, size = ENTRY.unpack_from(data, at)
        at += ENTRY.size
        path = data[at:at + pathlen].decode("utf-8", "replace")
        at += pathlen
        if kind != ENT_DIRTIME:
            for _ in range(nxattr):
                namelen, vlen = struct.unpack_from("<II", data, at)
                at += 8 + namelen + vlen
            body = data[at:at + size] if kind != ENT_DIR else b""
            at += size if kind != ENT_DIR else 0
        else:
            body = b""
        at += (-at) % 8
        if at > len(data):
            raise SystemExit("var-store-read: an entry runs past the archive")
        yield dict(kind=kind, mode=mode, path=path, size=size, mtime=sec, body=body)


def under(path, prefix):
    prefix = prefix.strip("/")
    return prefix == "" or path == prefix or path.startswith(prefix + "/")


def main(argv):
    image = None
    if len(argv) > 2 and argv[1] == "--image":
        image = argv[2]
        argv = argv[:1] + argv[3:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return selftest()
    src = Source(image)
    if cmd == "show":
        for slot in range(SLOTS):
            h = read_head(src, slot)
            print(f"slot {slot}: " + ("nothing good" if h is None else
                  f"generation {h['generation']}, {h['entries']} entries, {h['bytes']} bytes, written at {h['seconds']}"))
        return 0
    h, data = newest_archive(src)
    if h is None:
        print("var-store-read: no slot holds a good archive")
        return 1
    if cmd == "list":
        prefix = argv[2] if len(argv) > 2 else ""
        for e in entries(data):
            if e["kind"] != ENT_DIRTIME and under(e["path"], prefix):
                print(f"{e['kind']} {e['mode']:04o} {e['size']:10d} {e['path']}")
        return 0
    if cmd == "extract" and len(argv) == 4:
        prefix, dest = argv[2].strip("/"), argv[3]
        n = 0
        for e in entries(data):
            if not under(e["path"], prefix) or e["kind"] not in (ENT_DIR, ENT_FILE):
                continue
            rel = e["path"][len(prefix):].lstrip("/")
            if ".." in rel.split("/"):
                continue
            out = os.path.join(dest, rel) if rel else dest
            if e["kind"] == ENT_DIR:
                os.makedirs(out, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
                with open(out, "wb") as f:
                    f.write(e["body"])
                n += 1
        print(f"var-store-read: generation {h['generation']}: {n} files of {prefix or '/'} into {dest}")
        return 0
    print(__doc__)
    return 2


def selftest():
    def entry(kind, path, body=b"", mode=0o644, xattrs=()):
        x = b"".join(struct.pack("<II", len(k), len(v)) + k + v for k, v in xattrs)
        e = ENTRY.pack(kind, mode, 0, 0, 0, len(path), len(xattrs), 0, 1700000000, len(body) if kind != ENT_DIR else 0)
        b = e + path.encode() + (x if kind != ENT_DIRTIME else b"") + (body if kind not in (ENT_DIR, ENT_DIRTIME) else b"")
        return b

    stream = b""
    for piece in (entry(ENT_DIR, "", mode=0o755), entry(ENT_DIR, "db", mode=0o755),
                  entry(ENT_DIR, "db/hakios-metal-cache", mode=0o755),
                  entry(ENT_FILE, "db/hakios-metal-cache/0123.stage", b"HMCS" + bytes(range(37))),
                  entry(ENT_FILE, "db/hakios-metal-cache/4567.stage", b"HMCS", xattrs=((b"com.x", b"yz"),)),
                  entry(ENT_FILE, "db/other", b"not this one"),
                  entry(ENT_SYMLINK, "db/hakios-metal-cache/link", b"0123.stage"),
                  entry(ENT_DIRTIME, "db/hakios-metal-cache"), entry(ENT_DIRTIME, "db")):
        stream += piece + b"\0" * ((-len(piece)) % 8)
    def head(slot, generation, data, count=9):
        raw = HEAD.pack(MAGIC, 1, slot, generation, len(data), count, 1700000001, zlib.crc32(data) & 0xffffffff, 0)
        return HEAD.pack(MAGIC, 1, slot, generation, len(data), count, 1700000001, zlib.crc32(data) & 0xffffffff, head_crc(raw))
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        img = os.path.join(tmp, "store")
        with open(img, "wb") as f:
            # slot 0 an older generation, slot 1 the newest: the newest is read
            f.write(head(0, 1, b"", count=0).ljust(BLOCK, b"\0"))
            f.seek(SLOT_BYTES)
            f.write(head(1, 2, stream).ljust(BLOCK, b"\0") + stream)
        src = Source(img)
        h, _ = newest_archive(src)
        failures += h is None or h["slot"] != 1
        dest = os.path.join(tmp, "out")
        main(["x", "--image", img, "extract", "db/hakios-metal-cache", dest])
        got = sorted(os.listdir(dest))
        failures += got != ["0123.stage", "4567.stage"]
        failures += open(os.path.join(dest, "0123.stage"), "rb").read() != b"HMCS" + bytes(range(37))
        # a torn archive (one byte changed) is refused whole, not half read
        with open(img, "r+b") as f:
            f.seek(SLOT_BYTES + BLOCK + 60)
            f.write(b"\xff")
        try:
            stream_of(src, read_head(src, 1))
            failures += 1
        except BadArchive:
            pass
        h, data = newest_archive(src)
        failures += h is None or h["slot"] != 0 or data != b""
        # a header whose own checksum is wrong is not a slot
        with open(img, "r+b") as f:
            f.seek(SLOT_BYTES + 16)
            f.write(b"\x09")
        failures += read_head(src, 1) is not None
    print("var-store-read selftest: " + ("ok" if failures == 0 else f"{failures} failures"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
