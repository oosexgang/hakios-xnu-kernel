#!/usr/bin/env python3
"""Find the Metal libraries in an RE Engine pak, inside an app package, by content.

A game does not ship `.metallib` files. Resident Evil 4's are inside
`re_chunk_000.pak` -- Capcom's own archive, 383 MB of the `.ipa` -- and reach
Metal through `newLibraryWithData:`, so measuring them (docs/air.md section 8)
starts with reading that archive.

The format is KPKA, version 4: a 16-byte header (magic, version, file count, a
word) and one 48-byte record per file -- two name-hash words, the offset, the
compressed size, the uncompressed size, an attribute word whose low nibble is
the compression (0 stored, 1 deflate, 2 zstd) and whose bits 16..23 are an
encryption kind, and a checksum. Nothing of that is taken on faith: `check`
prints the four things that say the table was parsed rather than guessed at --
every extent inside the file, the data starting exactly where the table ends
and ending at the last byte, no partial overlaps, and every entry decompressing
to exactly the length its record claims.

The names are hashes and are not resolved here, which costs nothing: a Metal
library is findable by content. `libs` keeps every MTLB container whose header
is consistent (total size, function-table bounds), wherever in an entry it sits,
because a library may be packed inside a larger file of the engine's own.

Two things this is careful about, both because the input is a downloaded
package and not ours:

  - it reads the pak straight out of the zip in ONE FORWARD PASS. The table is
    at the head, so the entries can be walked in offset order, and nothing
    larger than a single entry is ever in memory -- 927 MB of entry payload
    goes through without a 927 MB file ever existing.
  - it executes nothing from the package and interprets nothing but the two
    formats above.

usage: repak.py check IPA MEMBER           the table and the checks
       repak.py libs IPA MEMBER OUTDIR     every distinct Metal library, written out
"""
import collections
import hashlib
import os
import struct
import sys
import zipfile
import zlib

ENT = 48


def open_member(ipa, member):
    """The pak as a forward-only stream, and its uncompressed length."""
    z = zipfile.ZipFile(ipa)
    return z.open(member), z.getinfo(member).file_size


def read_table(f):
    head = f.read(16)
    magic, ver, nfiles, word = struct.unpack("<4sIII", head)
    if magic != b"KPKA":
        raise SystemExit("repak: not a KPKA pak: %r" % magic)
    raw = f.read(nfiles * ENT)
    if len(raw) != nfiles * ENT:
        raise SystemExit("repak: the entry table is short")
    ents = []
    for i in range(nfiles):
        lo, hi, off, csz, dsz, attr, chk = struct.unpack_from("<IIQQQQQ", raw, i * ENT)
        ents.append(dict(i=i, lo=lo, hi=hi, off=off, csz=csz, dsz=dsz, attr=attr))
    return dict(version=ver, nfiles=nfiles, word=word), ents


def distinct(ents):
    """One entry per extent, in offset order.

    179 of RE4's 2 303 records are a second name for an extent another record
    already claims -- one blob is named 51 times -- which is what a
    deduplicating packer produces. The duplicates cost nothing and a forward
    pass cannot revisit them, so the extents are walked once each.
    """
    seen = {}
    for e in ents:
        seen.setdefault((e["off"], e["csz"]), e)
    return sorted(seen.values(), key=lambda e: e["off"])


def decode(e, blob):
    """One entry's bytes. Returns (data, how) or (None, why)."""
    kind = e["attr"] & 0xF
    if kind == 0:
        return blob, "stored"
    if kind == 1:
        for wbits, how in ((-15, "deflate"), (15, "zlib")):
            try:
                return zlib.decompress(blob, wbits), how
            except zlib.error:
                pass
        return None, "deflate failed"
    if kind == 2:
        try:
            import zstandard
        except ImportError:
            return None, "zstd entry and no zstandard module"
        try:
            return (zstandard.ZstdDecompressor()
                    .decompress(blob, max_output_size=e["dsz"] + 64), "zstd")
        except Exception as exc:                        # the library's own errors
            return None, "zstd failed: %s" % exc
    return None, "compression %d, which this does not know" % kind


def mtlb_length(data, pos):
    """The length of the MTLB container at pos, or None if that is not one.

    The same three header fields tools/metallib.py reads: a container says its
    own total size at 0x10 and its function table at 0x18, and a library always
    puts that table at 0x58.
    """
    if len(data) - pos < 0x58:
        return None
    total = struct.unpack_from("<Q", data, pos + 0x10)[0]
    funcs_off, funcs_end = struct.unpack_from("<QQ", data, pos + 0x18)
    if not 0x58 <= total <= len(data) - pos:
        return None
    if funcs_off != 0x58 or not funcs_off <= funcs_end <= total:
        return None
    return total


def walk(ipa, member, keep):
    """Every entry, decoded, in one forward pass. keep(entry, data) per entry."""
    f, size = open_member(ipa, member)
    stats, problems = collections.Counter(), collections.Counter()
    with f:
        hdr, ents = read_table(f)
        pos = 16 + hdr["nfiles"] * ENT
        ext = distinct(ents)
        stats["records"] = len(ents)
        stats["distinct extents"] = len(ext)
        for e in ext:
            if e["off"] < pos:
                problems["behind the cursor: extents overlap partially"] += 1
                continue
            while pos < e["off"]:
                skip = f.read(min(1 << 20, e["off"] - pos))
                if not skip:
                    break
                pos += len(skip)
            blob = f.read(e["csz"])
            pos += len(blob)
            if len(blob) != e["csz"]:
                problems["short read"] += 1
                continue
            data, how = decode(e, blob)
            stats[how] += 1
            if data is None:
                problems[how] += 1
                continue
            if len(data) != e["dsz"]:
                problems["length disagrees with the record"] += 1
            keep(e, data)
            del data, blob
    return hdr, ents, size, stats, problems


def check(ipa, member):
    hdr, ents, size, stats, problems = walk(ipa, member, lambda e, d: None)
    print("header: KPKA version %d, %d records, word 0x%08x"
          % (hdr["version"], hdr["nfiles"], hdr["word"]))
    print("compression:", dict(collections.Counter(e["attr"] & 0xF for e in ents)))
    print("encryption:", dict(collections.Counter((e["attr"] >> 16) & 0xFF for e in ents)))
    out = [e for e in ents if e["off"] < 16 or e["off"] + e["csz"] > size]
    srt = distinct(ents)
    partial = sum(1 for a, b in zip(srt, srt[1:]) if a["off"] + a["csz"] > b["off"])
    print("records whose extent leaves the %d-byte file: %d" % (size, len(out)))
    print("extents that overlap partially: %d" % partial)
    print("table ends at %d, first data at %d, last data ends at %d"
          % (16 + hdr["nfiles"] * ENT, srt[0]["off"], srt[-1]["off"] + srt[-1]["csz"]))
    print("decoded:", dict(stats))
    print("problems:", dict(problems) or "none")


def libs(ipa, member, outdir):
    os.makedirs(outdir, exist_ok=True)
    found = collections.Counter()
    seen = {}

    def keep(e, data):
        at = -1
        while True:
            at = data.find(b"MTLB", at + 1)
            if at < 0:
                return
            ln = mtlb_length(data, at)
            if ln is None:
                found["MTLB bytes that are not a container"] += 1
                continue
            found["containers"] += 1
            blob = data[at:at + ln]
            h = hashlib.sha256(blob).hexdigest()
            if h in seen:
                found["duplicates of one already written"] += 1
                continue
            seen[h] = True
            name = "e%05d_%08x%08x_%08x.metallib" % (e["i"], e["hi"], e["lo"], at)
            with open(os.path.join(outdir, name), "wb") as fp:
                fp.write(blob)
            found["written"] += 1

    hdr, ents, size, stats, problems = walk(ipa, member, keep)
    print("entries:", dict(stats))
    print("problems:", dict(problems) or "none")
    print("libraries:", dict(found))


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[-2].strip())
    if sys.argv[1] == "check":
        check(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "libs":
        libs(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit("repak: unknown command %r" % sys.argv[1])
