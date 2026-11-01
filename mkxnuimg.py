#!/usr/bin/env python3
"""
Pack a device's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-d device] [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PAYLOAD_MAX = 0x04000000

# What each device's boot chain wants: where its UEFI FD runs (the platform's MemoryMapLib, "UEFI FD"), the largest
# FD it may be, where the shim leaves the payload (a region its memory map reserves as "XNU Payload": the loader is
# built with the same value, LOADER_CFLAGS=-DPAYLOAD_BASE=...), and the boot image header ABL accepts.
DEVICES = {
    'redwood': dict(fd_base=0x9FD00000, fd_size=0x200000, payload_base=0xF0000000, header=1),
    # SM8475: Silicium's waipio platforms (ingres) run the FD at 0xA7100000 from a header-v4 image, and mayfly's ABL
    # ignores anything older: a v2 image is answered "OKAY" and Android boots instead (mayfly-hackios, measured).
    # 0xF0000000 is inside the TZ apps' carve-out here; the payload goes to the high DDR window, which has no
    # carve-out (the phone's own device tree). Both values are to be checked against mayflyPkg when it is built.
    # Its ABL also takes a "fastboot boot" image only when the header's OS version and patch level are set: with
    # them at 0 it answers OKAY and starts Android from boot_a instead (seen 2026-10-31). The values are the stock
    # boot.img's, neither older nor newer than what the phone runs.
    'mayfly': dict(fd_base=0xA7100000, fd_size=0x300000, payload_base=0xB0000000, header=4, shim_cflags='-DHAKIOS_GH_WDT_OFF',
                   image_size_covers_payload=True),
}

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=os.environ.get('DEVICE', 'redwood'), choices=sorted(DEVICES))
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()
dev = DEVICES[args.device]
FD_BASE, FD_SIZE, PAYLOAD_BASE = dev['fd_base'], dev['fd_size'], dev['payload_base']
PREBUILT = os.path.join(HERE, 'devices', args.device, 'prebuilt')
if args.output is None:
    args.output = os.path.join(HERE, f'xnu-{args.device}.img')

fd_path = os.path.join(MU, f'Build/{args.device}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x} ' + dev.get('shim_cflags', '') + ' ' + os.environ.get('SHIM_CFLAGS', '')],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{args.device}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
for name, path in parts.items():
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ("\n  The UEFI image is not in the repository: it carries the phone vendor's own drivers, which are not\n"
                    "  redistributed here. Build it from your own phone's firmware: uefi/README.md")
        sys.exit(f'mkxnuimg.py: {path} not found{hint}')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE if dev['header'] == 1 else 0 < len(fd) <= FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if dev.get('image_size_covers_payload'):
    # The shim's ARM64 Image header says the image is FD_SIZE long. mayfly's ABL takes a "fastboot boot" image whose
    # kernel is bigger than that as a bad one and starts Android instead (the same image without a payload is taken):
    # the header has to cover the payload too. Nothing in the shim reads this field.
    import struct
    blob = bytearray(blob)
    struct.pack_into('<Q', blob, 16, (len(blob) + 0xfff) & ~0xfff)
    blob = bytes(blob)

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', str(dev['header']), '-o', args.output] +
                          (['--os_version', dev['os_version'], '--os_patch_level', dev['os_patch_level']]
                           if 'os_version' in dev else []))
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
