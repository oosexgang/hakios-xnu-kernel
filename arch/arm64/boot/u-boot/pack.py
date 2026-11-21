#!/usr/bin/env python3
"""Pack Redwood U-Boot, loader.efi, and an optional XNUP payload for `fastboot boot`."""
import argparse
import gzip
import hashlib
import struct
import zlib
from pathlib import Path

TOP = Path(__file__).resolve().parents[4]
PAGE = 2048
BASE = 0x10000000
HKUB_MAGIC = b"HKUB"
HKUB_VERSION = 1
PAYLOAD_MAGIC = b"XNUP"
PAYLOAD_LIMIT = 0x04000000 - 0x100000


def hkub(entries):
    """Build U-Boot's HKUB ramdisk container, with 4 KiB aligned entries and CRC-32."""
    header_size = 16 + 32 * len(entries)
    first = (header_size + 0xFFF) & ~0xFFF
    table = bytearray()
    body = bytearray()
    for name, data in entries:
        encoded = name.encode("ascii")
        if len(encoded) >= 16:
            raise ValueError(f"HKUB entry name is too long: {name}")
        offset = first + len(body)
        table += struct.pack("<16sIIII", encoded, offset, len(data), zlib.crc32(data), 0)
        body += data
        body += b"\0" * (-len(body) % 0x1000)
    head = struct.pack("<4sIII", HKUB_MAGIC, HKUB_VERSION, len(entries), header_size) + table
    return head + b"\0" * (first - len(head)) + body


def boot_image(kernel, ramdisk):
    """Make Android boot-image header v1, as accepted by Redwood's ABL."""
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b""):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    sha.update(struct.pack("<I", 0))  # recovery DTBO size
    header = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), BASE + 0x8000,
        len(ramdisk), BASE + 0x01000000,
        0, 0, BASE + 0x100,
        PAGE, 1, 0,
        b"", b"", sha.digest(), b"",
    )
    header += struct.pack("<IQI", 0, 0, len(header) + 16)

    def pad(data):
        return data + b"\0" * (-len(data) % PAGE)

    return pad(header) + pad(kernel) + pad(ramdisk)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--u-boot", type=Path, default=TOP / "build/u-boot-redwood/u-boot.bin")
    p.add_argument("--loader", type=Path, default=TOP / "arch/arm64/boot/loader.efi")
    p.add_argument("--dtb", type=Path, default=TOP / "firmware/redwood/uefi/device-tree.dtb")
    p.add_argument("--env", type=Path, default=Path(__file__).resolve().parent / "env/redwood.env")
    p.add_argument("--payload", type=Path, help="optional XNUP payload")
    p.add_argument("-o", "--output", type=Path, default=TOP / "build/redwood-u-boot.img")
    a = p.parse_args()

    for path in (a.u_boot, a.loader, a.dtb, a.env):
        if not path.is_file():
            p.error(f"missing {path}")
    ub = a.u_boot.read_bytes()
    if len(ub) < 64 or ub[0x38:0x3C] != b"ARM\x64":
        p.error(f"{a.u_boot} is not an AArch64 Linux image (expected ARMd header)")
    loader = a.loader.read_bytes()
    if not loader.startswith(b"MZ"):
        p.error(f"{a.loader} is not a UEFI application")
    entries = [("loader", loader)]
    if a.payload:
        payload = a.payload.read_bytes()
        if len(payload) < 8 or payload[:4] != PAYLOAD_MAGIC:
            p.error(f"{a.payload} is not an XNUP payload")
        if len(payload) > PAYLOAD_LIMIT:
            p.error(f"payload exceeds the 63 MiB UEFI reservation ({len(payload)} bytes)")
        entries.insert(0, ("payload", payload))
    entries.append(("env", a.env.read_bytes()))
    ramdisk = hkub(entries)
    kernel = gzip.compress(ub, compresslevel=6, mtime=0) + a.dtb.read_bytes()
    image = boot_image(kernel, ramdisk)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(image)
    print(f"{a.output}: {len(image)} bytes; U-Boot {len(ub)}, loader {len(loader)}, "
          f"payload {len(entries[0][1]) if entries[0][0] == 'payload' else 0}, ramdisk {len(ramdisk)}")
    print(f"SHA256 {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    main()
