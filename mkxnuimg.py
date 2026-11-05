#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/redwood/prebuilt/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'devices', 'redwood', 'prebuilt')
FD_BASE, FD_SIZE = 0x9FD00000, 0x200000
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default='redwood', choices=['redwood', 'dipper'], help='target device')
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

if not args.output:
    args.output = os.path.join(HERE, f'xnu-{args.device}.img')

DEVICE_CONFIGS = {
    'dipper': {
        'fd_base': 0x9FC00000,
        'fd_size': 0x300000,
        'prebuilt_dir': os.path.join(HERE, 'devices', 'dipper', 'prebuilt'),
        'pkg': 'dipperPkg',
        'extra_boot_args': ['--base', '0x10000000', '--kernel_offset', '0x00000000',
                            '--ramdisk_offset', '0x00000000', '--tags_offset', '0x00000000',
                            '--os_version', '9.0.0', '--os_patch_level', '2020-09'],
    },
    'redwood': {
        'fd_base': 0x9FD00000,
        'fd_size': 0x200000,
        'prebuilt_dir': os.path.join(HERE, 'devices', 'redwood', 'prebuilt'),
        'pkg': 'redwoodPkg',
        'extra_boot_args': [],
    }
}

cfg = DEVICE_CONFIGS[args.device]
FD_BASE, FD_SIZE = cfg['fd_base'], cfg['fd_size']
PREBUILT = cfg['prebuilt_dir']

fd_path = os.path.join(MU, f'Build/{cfg["pkg"]}/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, 'Resources/DTBs/redwood.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
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
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    cmd = [sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
           '--header_version', '1', '-o', args.output]
    cmd.extend(cfg.get('extra_boot_args', []))
    subprocess.check_call(cmd)
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
