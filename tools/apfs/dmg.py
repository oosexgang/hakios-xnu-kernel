#!/usr/bin/env python3
# Read a UDIF disk image (.dmg): the block table in its XML property list, and the
# compressed chunks it points at.  Enough of the format to get at what is inside:
# random reads anywhere in the decompressed image, and a whole-image extract.
#
# A UDIF file is laid out back to front: the compressed data comes first, then an XML
# property list, then a 512-byte trailer ("koly") that says where the plist is.  The
# plist holds one "blkx" resource per partition -- each a "mish" block table whose
# entries map a run of output sectors to a run of bytes in the file.
#
# usage: dmg.py info IMAGE
#        dmg.py extract IMAGE OUT [--part N | --apfs]
#        dmg.py cat IMAGE OFFSET LENGTH        (hex dump, for poking at structures)

import base64
import bz2
import lzma
import plistlib
import struct
import sys
import zlib

SECTOR = 512

# chunk types, as they appear in a block table entry
CHUNK_ZERO = 0x00000000          # a run of zeros, no data in the file
CHUNK_RAW = 0x00000001           # stored as is
CHUNK_IGNORE = 0x00000002        # unallocated, reads as zeros
CHUNK_COMMENT = 0x7FFFFFFE
CHUNK_ADC = 0x80000004
CHUNK_ZLIB = 0x80000005
CHUNK_BZIP2 = 0x80000006
CHUNK_LZFSE = 0x80000007
CHUNK_LZMA = 0x80000008
CHUNK_LAST = 0xFFFFFFFF

CHUNK_NAMES = {
    CHUNK_ZERO: "zero", CHUNK_RAW: "raw", CHUNK_IGNORE: "ignore",
    CHUNK_COMMENT: "comment", CHUNK_ADC: "adc", CHUNK_ZLIB: "zlib",
    CHUNK_BZIP2: "bzip2", CHUNK_LZFSE: "lzfse", CHUNK_LZMA: "lzma",
    CHUNK_LAST: "last",
}


class Chunk:
    __slots__ = ("type", "sector", "count", "in_off", "in_len")

    def __init__(self, type_, sector, count, in_off, in_len):
        self.type = type_
        self.sector = sector        # first output sector, absolute
        self.count = count          # output sectors
        self.in_off = in_off        # byte offset in the file
        self.in_len = in_len        # bytes in the file

    @property
    def out_off(self):
        return self.sector * SECTOR

    @property
    def out_len(self):
        return self.count * SECTOR

    def __repr__(self):
        return "<chunk %s out=%d+%d in=%d+%d>" % (
            CHUNK_NAMES.get(self.type, hex(self.type)),
            self.out_off, self.out_len, self.in_off, self.in_len)


class Blkx:
    """One block table: the chunks of one partition (or of the whole image)."""

    def __init__(self, name, id_, blob):
        self.name = name
        self.id = id_
        magic, ver, sector, count, data_off, _bufs, _descs = struct.unpack_from(
            ">4sIQQQII", blob, 0)
        if magic != b"mish":
            raise ValueError("block table %r: bad magic %r" % (name, magic))
        self.version = ver
        self.sector = sector        # where this partition starts in the image
        self.count = count          # its length in sectors
        nchunks = struct.unpack_from(">I", blob, 0xC8)[0]
        self.chunks = []
        off = 0xCC
        for _ in range(nchunks):
            t, _comment, csec, ccnt, coff, clen = struct.unpack_from(">IIQQQQ", blob, off)
            off += 40
            if t in (CHUNK_LAST, CHUNK_COMMENT):
                continue
            # a chunk's sector number is relative to the table, its file offset to data_off
            self.chunks.append(Chunk(t, sector + csec, ccnt, data_off + coff, clen))

    @property
    def out_off(self):
        return self.sector * SECTOR

    @property
    def out_len(self):
        return self.count * SECTOR


def _adc_decompress(src, out_len):
    """Apple Data Compression: a byte-oriented LZ with three kinds of opcode."""
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < out_len:
        b = src[i]
        if b & 0x80:                          # literal run, 1..128 bytes
            n = (b & 0x7F) + 1
            out += src[i + 1:i + 1 + n]
            i += 1 + n
        elif b & 0x40:                        # long match: 4..67 bytes, distance <= 65535
            n = (b & 0x3F) + 4
            dist = (src[i + 1] << 8) | src[i + 2]
            i += 3
            for _ in range(n):
                out.append(out[-dist - 1])
        else:                                 # short match: 3..10 bytes, distance <= 1023
            n = ((b & 0x3C) >> 2) + 3
            dist = ((b & 0x03) << 8) | src[i + 1]
            i += 2
            for _ in range(n):
                out.append(out[-dist - 1])
    return bytes(out[:out_len])


def _lzfse_decompress(src, out_len):
    try:
        import lzfse
    except ImportError:
        raise SystemExit("dmg.py: this image uses LZFSE chunks; install the python "
                         "'lzfse' module (pip install lzfse)")
    return lzfse.decompress(src)[:out_len]


class Dmg:
    """A UDIF image, readable at any offset of its decompressed contents."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.file_size = self.f.tell()
        self._read_trailer()
        self._read_plist()
        # one cached decompressed chunk: reads walk forward, so this is nearly always a hit
        self._cache_key = None
        self._cache_buf = None

    def _read_trailer(self):
        self.f.seek(self.file_size - 512)
        k = self.f.read(512)
        if k[:4] != b"koly":
            raise ValueError("%s: no UDIF trailer (koly) at the end of the file" % self.path)
        self.version = struct.unpack_from(">I", k, 4)[0]
        self.flags = struct.unpack_from(">I", k, 0xC)[0]
        self.data_fork_off, self.data_fork_len = struct.unpack_from(">QQ", k, 0x18)
        self.rsrc_fork_off, self.rsrc_fork_len = struct.unpack_from(">QQ", k, 0x28)
        self.xml_off, self.xml_len = struct.unpack_from(">QQ", k, 0xD8)
        self.image_variant = struct.unpack_from(">I", k, 0x1E8)[0]
        self.sector_count = struct.unpack_from(">Q", k, 0x1EC)[0]

    def _read_plist(self):
        if not self.xml_len:
            raise ValueError("%s: no XML property list; a resource fork table is not "
                             "supported" % self.path)
        self.f.seek(self.xml_off)
        pl = plistlib.loads(self.f.read(self.xml_len))
        self.tables = []
        for ent in pl["resource-fork"]["blkx"]:
            data = ent["Data"]
            if isinstance(data, str):
                data = base64.b64decode(data)
            self.tables.append(Blkx(ent.get("Name", ""), ent.get("ID", ""), data))
        self.tables.sort(key=lambda b: b.sector)
        # one flat chunk list for the whole image, so a read need not care about partitions
        self._chunks = []
        for b in self.tables:
            self._chunks.extend(b.chunks)
        self._chunks.sort(key=lambda c: c.sector)
        self._starts = [c.out_off for c in self._chunks]

    @property
    def size(self):
        return self.sector_count * SECTOR

    def _chunk_data(self, c):
        """The decompressed bytes of one chunk."""
        if self._cache_key == c.in_off and self._cache_buf is not None:
            return self._cache_buf
        if c.type in (CHUNK_ZERO, CHUNK_IGNORE):
            buf = b"\0" * c.out_len
        else:
            self.f.seek(c.in_off)
            raw = self.f.read(c.in_len)
            if c.type == CHUNK_RAW:
                buf = raw
            elif c.type == CHUNK_ZLIB:
                buf = zlib.decompress(raw)
            elif c.type == CHUNK_BZIP2:
                buf = bz2.decompress(raw)
            elif c.type == CHUNK_LZMA:
                buf = lzma.decompress(raw)
            elif c.type == CHUNK_LZFSE:
                buf = _lzfse_decompress(raw, c.out_len)
            elif c.type == CHUNK_ADC:
                buf = _adc_decompress(raw, c.out_len)
            else:
                raise ValueError("unknown chunk type 0x%08x at output offset %d"
                                 % (c.type, c.out_off))
            if len(buf) != c.out_len:
                raise ValueError("chunk at output offset %d: %d bytes out of %s, "
                                 "expected %d" % (c.out_off, len(buf),
                                                  CHUNK_NAMES.get(c.type, "?"), c.out_len))
        self._cache_key, self._cache_buf = c.in_off, buf
        return buf

    def read(self, off, length):
        """Read the decompressed image. Holes between chunks read as zeros."""
        import bisect
        out = bytearray()
        end = off + length
        while off < end:
            i = bisect.bisect_right(self._starts, off) - 1
            c = self._chunks[i] if i >= 0 else None
            if c is None or off >= c.out_off + c.out_len:
                # before the first chunk, or in a hole: zeros up to the next chunk
                nxt = self._starts[i + 1] if i + 1 < len(self._starts) else end
                n = min(end, nxt) - off
                out += b"\0" * n
                off += n
                continue
            buf = self._chunk_data(c)
            inner = off - c.out_off
            n = min(end - off, c.out_len - inner)
            out += buf[inner:inner + n]
            off += n
        return bytes(out)

    def extract(self, out_path, start=0, length=None, progress=None):
        """Write a byte range of the decompressed image to a file."""
        if length is None:
            length = self.size - start
        end = start + length
        written = 0
        with open(out_path, "wb") as g:
            # walk the chunks that overlap the range, so nothing is decompressed twice
            import bisect
            i = max(0, bisect.bisect_right(self._starts, start) - 1)
            pos = start
            while pos < end:
                c = self._chunks[i] if i < len(self._chunks) else None
                if c is None or c.out_off >= end:
                    g.write(b"\0" * (end - pos))
                    written += end - pos
                    break
                if pos < c.out_off:                 # hole before this chunk
                    n = c.out_off - pos
                    g.write(b"\0" * n)
                    written += n
                    pos = c.out_off
                if pos >= c.out_off + c.out_len:
                    i += 1
                    continue
                buf = self._chunk_data(c)
                inner = pos - c.out_off
                n = min(end - pos, c.out_len - inner)
                g.write(buf[inner:inner + n])
                written += n
                pos += n
                if progress and (i & 0x3FF) == 0:
                    progress(written, length)
                i += 1
        if progress:
            progress(written, length)
        return written


# ---- GPT, just enough to find the partitions in an extracted whole-disk image ----

APFS_PART_TYPE = "7C3457EF-0000-11AA-AA11-00306543ECAC"


def _guid(b):
    """A GPT type GUID: three little-endian fields then eight bytes as they lie."""
    d1, d2, d3 = struct.unpack_from("<IHH", b, 0)
    return "%08X-%04X-%04X-%s-%s" % (
        d1, d2, d3, b[8:10].hex().upper(), b[10:16].hex().upper())


class Partition:
    def __init__(self, index, type_guid, part_guid, first, last, name):
        self.index = index
        self.type_guid = type_guid
        self.guid = part_guid
        self.first_lba = first
        self.last_lba = last
        self.name = name

    @property
    def offset(self):
        return self.first_lba * SECTOR

    @property
    def length(self):
        return (self.last_lba - self.first_lba + 1) * SECTOR


def read_gpt(reader):
    """Partitions of a GPT, from anything with a read(off, len). [] if there is no GPT."""
    hdr = reader.read(SECTOR, SECTOR)
    if hdr[:8] != b"EFI PART":
        return []
    part_lba, nparts, entsz = struct.unpack_from("<QII", hdr, 72)
    tbl = reader.read(part_lba * SECTOR, nparts * entsz)
    out = []
    for i in range(nparts):
        e = tbl[i * entsz:(i + 1) * entsz]
        t = _guid(e[0:16])
        if t == "00000000-0000-0000-0000-000000000000":
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le", "replace").rstrip("\0")
        out.append(Partition(i, t, _guid(e[16:32]), first, last, name))
    return out


def _fmt(n):
    return "%d (%.2f GB)" % (n, n / 1e9) if n >= 1 << 30 else "%d (%.1f MB)" % (n, n / 1e6)


def cmd_info(path):
    d = Dmg(path)
    print("%s: %d bytes on disk" % (path, d.file_size))
    print("UDIF version %d, flags 0x%x, image variant %d" % (d.version, d.flags, d.image_variant))
    print("data fork  off=%d len=%d" % (d.data_fork_off, d.data_fork_len))
    print("xml plist  off=%d len=%d" % (d.xml_off, d.xml_len))
    print("image size %s in %d sectors" % (_fmt(d.size), d.sector_count))
    print()
    kinds = {}
    for c in d._chunks:
        k = kinds.setdefault(CHUNK_NAMES.get(c.type, hex(c.type)), [0, 0, 0])
        k[0] += 1
        k[1] += c.in_len
        k[2] += c.out_len
    print("%d block tables, %d chunks:" % (len(d.tables), len(d._chunks)))
    for name, (n, cin, cout) in sorted(kinds.items()):
        print("  %-8s %7d chunks  %14d -> %14d bytes" % (name, n, cin, cout))
    print()
    print("block tables:")
    for b in d.tables:
        print("  id=%-4s out=%d+%d (%s)  %d chunks  %s"
              % (b.id, b.out_off, b.out_len, _fmt(b.out_len), len(b.chunks), b.name))
    print()
    parts = read_gpt(d)
    if not parts:
        print("no GPT at sector 1 -- the image is a bare file system, not a whole disk")
        return
    print("GPT, %d partitions:" % len(parts))
    for p in parts:
        tag = "  <- APFS" if p.type_guid == APFS_PART_TYPE else ""
        print("  %d  lba %d..%d  off=%d len=%s  %-24s %s%s"
              % (p.index, p.first_lba, p.last_lba, p.offset, _fmt(p.length),
                 p.name, p.type_guid, tag))


def cmd_extract(path, out, which):
    d = Dmg(path)
    start, length, what = 0, d.size, "the whole image"
    if which is not None:
        parts = read_gpt(d)
        if not parts:
            raise SystemExit("dmg.py: no GPT in this image, nothing to pick from")
        if which == "apfs":
            cand = [p for p in parts if p.type_guid == APFS_PART_TYPE]
            if not cand:
                raise SystemExit("dmg.py: no APFS partition in the GPT")
            p = cand[0]
        else:
            sel = [p for p in parts if p.index == int(which)]
            if not sel:
                raise SystemExit("dmg.py: no partition %s" % which)
            p = sel[0]
        start, length = p.offset, p.length
        what = "partition %d (%s)" % (p.index, p.name)

    def prog(done, total):
        pct = 100.0 * done / total if total else 100.0
        sys.stderr.write("\r  %s: %6.2f%%  %d/%d bytes" % (what, pct, done, total))
        sys.stderr.flush()

    n = d.extract(out, start, length, prog)
    sys.stderr.write("\n")
    print("%s -> %s: %d bytes from offset %d" % (path, out, n, start))
    head = d.read(start, 64)
    print("first 32 bytes: %s" % head[:32].hex())
    # an APFS container starts with an object header; its magic is 32 bytes in
    if head[32:36] == b"NXSB":
        print("magic at offset 32: NXSB -- an APFS container")


def cmd_cat(path, off, length):
    d = Dmg(path)
    buf = d.read(off, length)
    for i in range(0, len(buf), 16):
        row = buf[i:i + 16]
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
        print("%012x  %-47s  %s" % (off + i, row.hex(" "), txt))


def main(argv):
    if len(argv) < 3:
        print(__doc__ or "", file=sys.stderr)
        print("usage: dmg.py info IMAGE\n"
              "       dmg.py extract IMAGE OUT [--part N | --apfs]\n"
              "       dmg.py cat IMAGE OFFSET LENGTH", file=sys.stderr)
        return 2
    cmd = argv[1]
    if cmd == "info":
        cmd_info(argv[2])
    elif cmd == "extract":
        which = None
        for a in argv[4:]:
            if a == "--apfs":
                which = "apfs"
            elif a.startswith("--part"):
                which = a.split("=", 1)[1] if "=" in a else argv[argv.index(a) + 1]
        cmd_extract(argv[2], argv[3], which)
    elif cmd == "cat":
        cmd_cat(argv[2], int(argv[3], 0), int(argv[4], 0))
    else:
        raise SystemExit("dmg.py: unknown command %r" % cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
