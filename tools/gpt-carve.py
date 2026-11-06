#!/usr/bin/env python3
"""
Take space off the end of a partition and put a new one in its place, in a GPT read from the
phone (tools/gpt-carve.py <gpt-primary.bin> <gpt-secondary.bin> <total-sectors>). Writes the new
primary and secondary images beside the originals; nothing touches the phone.

Here it is for the X6826B: 4 GiB off the end of userdata becomes "xnusys", where
userland/system.packfs lives so that mtkloader can read it straight off the flash (the eMMC
reader in mtkloader/msdc.h). Everything else -- every other entry, the disk GUID, the entry
order -- is left exactly as it was.
"""
import binascii
import struct
import sys
import uuid

SECTOR = 512
SHRINK = "userdata"
NEW_NAME = "xnusys"
NEW_SECTORS = 8 * 1024 * 1024          # 4 GiB
NEW_TYPE = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")   # Linux filesystem data
ALIGN = 2048                            # 1 MiB


def crc32(b):
    return binascii.crc32(b) & 0xffffffff


def name_of(entry):
    return entry[56:128].decode("utf-16-le").rstrip("\0")


def parse(hdr):
    sig, rev, hsize, hcrc, _, mylba, altlba, first, last = struct.unpack_from("<8sIIIIQQQQ", hdr, 0)
    guid = hdr[56:72]
    entlba, nent, entsize, entcrc = struct.unpack_from("<QIII", hdr, 72)
    assert sig == b"EFI PART", "not a GPT header"
    return dict(rev=rev, hsize=hsize, mylba=mylba, altlba=altlba, first=first, last=last,
                guid=guid, entlba=entlba, nent=nent, entsize=entsize, hcrc=hcrc, entcrc=entcrc)


def build_header(h, entries, mylba, altlba, entlba):
    out = bytearray(h["hsize"])
    struct.pack_into("<8sIIII", out, 0, b"EFI PART", h["rev"], h["hsize"], 0, 0)
    struct.pack_into("<QQQQ", out, 24, mylba, altlba, h["first"], h["last"])
    out[56:72] = h["guid"]
    struct.pack_into("<QIII", out, 72, entlba, h["nent"], h["entsize"], crc32(entries))
    struct.pack_into("<I", out, 16, crc32(bytes(out)))
    return bytes(out) + b"\0" * (SECTOR - h["hsize"] % SECTOR if h["hsize"] % SECTOR else 0)


def main():
    pri_path, sec_path, total = sys.argv[1], sys.argv[2], int(sys.argv[3])
    pri = bytearray(open(pri_path, "rb").read())
    sec = bytearray(open(sec_path, "rb").read())
    h = parse(pri[SECTOR:2 * SECTOR])
    esz, nent = h["entsize"], h["nent"]
    ents = pri[h["entlba"] * SECTOR: h["entlba"] * SECTOR + nent * esz]
    assert crc32(bytes(ents)) == h["entcrc"], "the entry array does not match its checksum"
    assert h["altlba"] == total - 1, f"header says the backup is at {h['altlba']}, disk ends at {total - 1}"

    entries = [bytearray(ents[i * esz:(i + 1) * esz]) for i in range(nent)]
    used = [(name_of(e), *struct.unpack_from("<QQ", e, 32)) for e in entries if e[0:16] != b"\0" * 16]
    print(f"{len(used)} partitions, last usable sector {h['last']}")

    victim = next(i for i, e in enumerate(entries) if name_of(e) == SHRINK)
    free = next(i for i, e in enumerate(entries) if e[0:16] == b"\0" * 16)
    vstart, vend = struct.unpack_from("<QQ", entries[victim], 32)
    print(f"{SHRINK}: {vstart}..{vend} ({(vend - vstart + 1) * SECTOR / 2**30:.1f} GiB), entry {victim}")

    new_start = (vend + 1 - NEW_SECTORS) & ~(ALIGN - 1)
    new_end = vend
    assert new_start > vstart + ALIGN, "not enough room in the partition being shrunk"
    for nm, s, e in used:
        assert nm == SHRINK or e < new_start or s > new_end, f"{nm} is in the way ({s}..{e})"

    struct.pack_into("<QQ", entries[victim], 32, vstart, new_start - 1)
    e = entries[free]
    e[0:16] = NEW_TYPE.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", e, 32, new_start, new_end, 0)
    e[56:128] = NEW_NAME.encode("utf-16-le").ljust(72, b"\0")

    print(f"{SHRINK}: {vstart}..{new_start - 1} ({(new_start - vstart) * SECTOR / 2**30:.1f} GiB)")
    print(f"{NEW_NAME}:   {new_start}..{new_end} ({(new_end - new_start + 1) * SECTOR / 2**30:.1f} GiB), entry {free}")

    new_ents = b"".join(bytes(x) for x in entries)
    pri[h["entlba"] * SECTOR: h["entlba"] * SECTOR + len(new_ents)] = new_ents
    pri[SECTOR:2 * SECTOR] = build_header(h, new_ents, 1, total - 1, h["entlba"])

    sec_entlba = total - 1 - (nent * esz // SECTOR)         # where the backup array sits
    off = len(sec) - SECTOR - nent * esz                     # its offset inside this image
    assert off >= 0, "the backup image is shorter than its entry array"
    sec[off:off + len(new_ents)] = new_ents
    sec[len(sec) - SECTOR:] = build_header(h, new_ents, total - 1, 1, sec_entlba)

    open(pri_path + ".new", "wb").write(bytes(pri))
    open(sec_path + ".new", "wb").write(bytes(sec))
    print(f"written: {pri_path}.new (sectors 0..{len(pri) // SECTOR - 1}), "
          f"{sec_path}.new (sectors {total - len(sec) // SECTOR}..{total - 1})")


if __name__ == "__main__":
    main()
