#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to the payload address.

What is the phone's comes from its Silicium config, devices/<device>/uefi/files/Resources/Configs/<device>.toml:
the FD's base and size ([uefi_fd]), the boot image header ([boot_image]) and, where 0xF0000000 is not free on that
phone, the payload's address ([hakios] payload_base; the loader's platform has the same).

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-d device] [-p payload.bin] [-o out.img]      (device: $DEVICE, else redwood)
"""
import argparse, gzip, os, subprocess, sys, tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PAYLOAD_MAX = 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=os.environ.get('DEVICE') or 'redwood')
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()
dev = args.device
args.output = args.output or os.path.join(HERE, f'xnu-{dev}.img')
PREBUILT = os.path.join(HERE, 'devices', dev, 'prebuilt')

toml_path = os.path.join(HERE, 'devices', dev, 'uefi', 'files', 'Resources', 'Configs', f'{dev}.toml')
if not os.path.isfile(toml_path):
    sys.exit(f'mkxnuimg.py: {toml_path} not found (is devices/{dev} checked out, with a uefi/ port?)')
cfg = tomllib.load(open(toml_path, 'rb'))
FD_BASE, FD_SIZE = cfg['uefi_fd']['base'], cfg['uefi_fd']['size']
HEADER_VERSION = cfg.get('boot_image', {}).get('header_version', 1)
KERNEL_HEADER = 1 if cfg.get('boot_shim', {}).get('requires_kernel_header', True) else 0
PAYLOAD_BASE = cfg.get('hakios', {}).get('payload_base', 0xF0000000)
assert PAYLOAD_BASE & 0xFFFF == 0 and PAYLOAD_BASE < 1 << 32, 'the shim loads payload_base as a 16-bit immediate << 16'

fd_path = os.path.join(MU, f'Build/{dev}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', f'REQUIRES_KERNEL_HEADER={KERNEL_HEADER}', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{dev}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
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
                    f"  redistributed here. Build it from your own phone's firmware: uefi/README.md (python3 build_uefi.py -d {dev})")
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
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', str(HEADER_VERSION), '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)}; '
      f'{dev}: FD at {FD_BASE:#x}, payload at {PAYLOAD_BASE:#x}, boot image v{HEADER_VERSION})')
