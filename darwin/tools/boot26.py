#!/usr/bin/env python3
"""Boot Darwin 27 (default), or Darwin 26 with --system/--root, on a phone from RAM (--board NAME: the phone's
package, brought in with tools/device.py add; not needed when only one phone's package is here).
No flashing, erasing or storage commands: stock fastboot boots the resident loader,
which receives the system image, kernel and root image over USB. Reboot restores Android.

Start with the phone in stock fastboot. Inspect the screen and loader log to verify
that the requested RAM run actually boots; a successful USB command is not boot proof."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DK = Path(__file__).resolve().parents[3]
DR = DK / 'darwin-renoir'
DARWIN_FW = DK / 'darwin27'                      # Darwin 27.2 (24B119); darwin26 (23G90) takes --system/--root
sys.path.insert(0, str(DR / 'recovery'))
from payload import prepare, validate_apfs, validate_mac  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import identity  # noqa: E402  (tools/identity.py: this phone's seed)
import boards  # noqa: E402  (arch/arm64/boot/dts/NAME/board.dts: what the phone must say it is, its default boot-args)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--board', default=os.environ.get('HAKIOS_BOARD'),
               help='the phone, by its package\'s name (tools/boards.py list). Without it: $HAKIOS_BOARD, or the '
                    'one package here that has a boot { } section')
p.add_argument('--wifi-mac', required=True, metavar='MAC', help="this handset's device Wi-Fi MAC")
p.add_argument('--kernel', type=Path,
               default=DK / 'xnu-12377-hakios/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837')
p.add_argument('--system', type=Path, default=DARWIN_FW / 'system27-prune.apfs')
p.add_argument('--root', type=Path, default=DARWIN_FW / 'root26-darwin.apfs')
p.add_argument('--boot-arg', action='append', default=[], metavar='NAME[=VALUE]',
               help='add (or replace) one boot argument, e.g. cpus=1; repeatable')
p.add_argument('--return-after', type=int, metavar='SECONDS',
               help='hand the phone back to the loader after this long (its log can then be fetched)')
options = p.parse_args()

fb = os.environ.get('FASTBOOT') or next((str(f) for f in (Path('C:/platform-tools/fastboot.exe'),
                                                         Path('/mnt/c/platform-tools/fastboot.exe')) if f.is_file()),
                                        None) or shutil.which('fastboot')
if not fb:
    raise SystemExit('Install platform-tools or set FASTBOOT')


def file(path):
    path = str(path)
    if fb.endswith('.exe') and os.name != 'nt':
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip()
    return path


def cmd(*args, serial=None, timeout=30, live=False):
    command = [fb] + (['-s', serial] if serial else []) + list(args)
    try:
        return subprocess.run(command, capture_output=not live, text=True, timeout=timeout, check=True)
    except subprocess.CalledProcessError as error:
        detail = '\n'.join(part.strip() for part in (error.stdout, error.stderr) if part and part.strip())
        raise SystemExit(f'fastboot {args[0]} failed (exit {error.returncode}).\n{detail}') from None
    except subprocess.TimeoutExpired:
        raise SystemExit(f'fastboot {args[0]} timed out after {timeout} seconds') from None


def devices():
    return [parts[0] for line in cmd('devices').stdout.splitlines()
            if len(parts := line.split()) >= 2 and parts[1] == 'fastboot']


def check(serial, key, value):
    q = cmd('getvar', key, serial=serial)
    match = re.search(r'(?m)^\s*(?:\(bootloader\)\s*)?' + re.escape(key) + r':\s*(\S+)\s*$', q.stdout + q.stderr)
    if match is None or match[1] != value:
        raise SystemExit(f'Wrong device: expected {key} {value}')


# which phone, and the boot arguments of its board with this run's own (tools/boards.py; tested there)
options.board, board = boards.board_to_boot(boards.load()[1], options.board)
boot = board['boot']
boot_args = boards.boot_arguments(board, options.boot_arg + (
    [f'loader_return_after={options.return_after}'] if options.return_after else []))
# this phone's identity (tools/identity.py): the same serial numbers and addresses at every boot
if not any(arg.startswith('identity=') for arg in boot_args):
    boot_args.append(f'identity={identity.seed_of(options.board):016x}')
loader = DR / f'prebuilt/{options.board}/resident-usb.img'
share = DR / f'recovery/assets/{options.board}-darwin-share.bin'
for path in (options.kernel, options.system, options.root, loader, share):
    if not path.is_file():
        raise SystemExit(f'Missing {path} (Documentation/darwin26-boot.md says how it is made)')
try:
    with options.system.open('rb') as image:
        validate_apfs(image.read(4096), options.system.stat().st_size)
    data = prepare(share.read_bytes(), time.time(),
                   wifi_mac=validate_mac(options.wifi_mac), kernel=options.kernel.read_bytes(), cpu_count=boot['cpus'],
                   ramdisk=options.root.read_bytes(), boot_args=boot_args)
except (OSError, ValueError) as error:
    raise SystemExit(str(error))

dev = devices()
if len(dev) != 1:
    raise SystemExit('Connect exactly one phone in fastboot mode')
if dev[0] == 'xnuloader':
    raise SystemExit('Restart the phone into stock fastboot first; resident RAM may be in use')
check(dev[0], 'product', boot['fastboot_product'])
cmd('boot', file(loader), serial=dev[0])
for _ in range(60):
    if 'xnuloader' in devices():
        break
    time.sleep(1)
else:
    raise SystemExit('Resident loader did not appear; do not flash anything')
check('xnuloader', 'board', options.board)
cmd('oem', 'ramdisk', serial='xnuloader')
print(f'Uploading the Darwin system image ({options.system.stat().st_size / 1e9:.2f} GB)...', flush=True)
cmd('stage', file(options.system), serial='xnuloader', timeout=900, live=True)
data = prepare(data, time.time())                       # the clock again, after the long upload
run = DARWIN_FW / 'run26-current.bin'
run.write_bytes(data)
cmd('stage', file(run), serial='xnuloader', timeout=180, live=True)
cmd('oem', 'run', serial='xnuloader')
print('RAM run requested. Inspect the screen/log to verify startup. Nothing was flashed or erased.')
