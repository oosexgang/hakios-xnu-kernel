#!/usr/bin/env python3
"""Lay out an EFI system partition for HakiOS: the loader, and the payload it reads itself.

usage: tools/mkesp.py OUTDIR [PAYLOAD] [--loader FILE]

  OUTDIR   the tree to make (it must be empty or not exist): copy it to a FAT32 volume, or serve it to QEMU
  PAYLOAD  the "XNUP" payload made by xnuloader/mkpayload.py (default: qemu/payload.bin, which `make qemu-open` makes)
  --loader the boot loader (default: xnuloader/loader.efi, else prebuilt/loader.efi)

What it makes:
  OUTDIR/EFI/BOOT/BOOTAA64.EFI     the loader, in the place an AArch64 UEFI boots a removable volume from
  OUTDIR/EFI/hakios/payload.bin    the payload: the kernel, the root image, the command line, kernel.ver
  OUTDIR/EFI/hakios/kernel.ver     the version, as text, so that a person can see what a volume holds

The loader reads \\EFI\\hakios\\payload.bin from the volume it was started from when nobody has put a payload in
memory (xnuloader/loader.c, load_payload_from_volume). That is UEFI-native boot: a boot menu, a USB stick or an SD
card starts the kernel, with no fastboot. docs/uefi-boot.md says what this does and does not do on a phone.

It refuses what the loader would refuse, and says so here instead of on a phone's screen: a file that is not a
payload (no XNUP), a payload with no kernel in it, one that does not fit in the loader's payload region (63 MiB: the
big Darwin system image does not come this way).
Truncated headers, entry data or padding, a missing end entry, and an empty kernel are refused before
anything is written to OUTDIR.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAGIC = 0x50554E58                              # "XNUP"
PAYLOAD_LIMIT = 0x04000000 - 0x100000           # the loader's payload region less the MiB of the panic log


def entries(data):
    """{name: bytes} of an XNUP payload, or a SystemExit with the reason."""
    if len(data) < 8 or struct.unpack_from("<I", data)[0] != MAGIC:
        sys.exit("mkesp: not a payload: it does not start with XNUP (make one with xnuloader/mkpayload.py)")
    size = struct.unpack_from("<I", data, 4)[0]
    end = 8 + size
    if end > len(data):
        sys.exit("mkesp: truncated payload: the header declares %d bytes, but the file holds %d" % (end, len(data)))
    out, at = {}, 8
    while at < end:
        if at + 24 > end:
            sys.exit("mkesp: truncated payload entry header at byte %d" % at)
        name = data[at:at + 16].split(b"\0")[0].decode(errors="replace")
        length = struct.unpack_from("<I", data, at + 16)[0]
        if not name:
            return out
        padded = (length + 15) & ~15
        if at + 24 + length > end:
            sys.exit("mkesp: payload entry %r extends past the declared payload" % name)
        if at + 24 + padded > end:
            sys.exit("mkesp: truncated padding for payload entry %r" % name)
        out[name] = data[at + 24:at + 24 + length]
        at += 24 + padded
    sys.exit("mkesp: payload has no end entry")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("payload", nargs="?", default=os.path.join(ROOT, "qemu", "payload.bin"))
    ap.add_argument("--loader")
    args = ap.parse_args()

    loader = args.loader
    if not loader:
        loader = os.path.join(ROOT, "xnuloader", "loader.efi")
        if not os.path.isfile(loader):
            loader = os.path.join(ROOT, "prebuilt", "loader.efi")
    for what, path in (("loader", loader), ("payload", args.payload)):
        if not os.path.isfile(path):
            sys.exit("mkesp: no %s at %s" % (what, path))
    if os.path.exists(args.outdir) and os.listdir(args.outdir):
        sys.exit("mkesp: %s is not empty" % args.outdir)

    data = open(args.payload, "rb").read()
    if len(data) > PAYLOAD_LIMIT:
        sys.exit("mkesp: the payload is %d bytes and the loader's payload region holds %d: the root image is too big for "
                 "this way of booting (the big Darwin image goes in over USB, `xl ramdisk`)" % (len(data), PAYLOAD_LIMIT))
    parts = entries(data)
    if not parts.get("kernel"):
        sys.exit("mkesp: the payload has no kernel data in it (entries: %s)" % (", ".join(sorted(parts)) or "none"))
    version = parts.get("kernel.ver", b"").rstrip(b"\0").decode(errors="replace")
    head = open(loader, "rb").read(2)
    if head != b"MZ":
        sys.exit("mkesp: %s is not an EFI application (no MZ)" % loader)

    boot, hakios = (os.path.join(args.outdir, "EFI", d) for d in ("BOOT", "hakios"))
    os.makedirs(boot, exist_ok=True)
    os.makedirs(hakios, exist_ok=True)
    shutil.copyfile(loader, os.path.join(boot, "BOOTAA64.EFI"))
    open(os.path.join(hakios, "payload.bin"), "wb").write(data)
    open(os.path.join(hakios, "kernel.ver"), "w", newline="\n").write((version or "unknown") + "\n")

    print("mkesp: %s" % args.outdir)
    print("  EFI/BOOT/BOOTAA64.EFI    %9d bytes  the loader" % os.path.getsize(loader))
    print("  EFI/hakios/payload.bin   %9d bytes  %s" % (len(data), ", ".join("%s%s" % (k, " (%d)" % len(v) if k != "kernel.ver" else "")
                                                                          for k, v in sorted(parts.items()))))
    print("  kernel.ver=%s" % (version or "unknown (the payload was made with --no-version)"))


if __name__ == "__main__":
    main()
