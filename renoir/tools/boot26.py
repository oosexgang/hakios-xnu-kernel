#!/usr/bin/env python3
"""Boot iOS 27 (default), or iOS 26 with --system/--root, on Renoir from RAM.
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
IOS = DK / 'ios27'                      # iOS 27.2 (24B119); ios26 (23G90) takes --system/--root
sys.path.insert(0, str(DR / 'recovery'))
from payload import prepare, validate_apfs, validate_mac  # noqa: E402

# Renoir's CPU/software-rendering configuration: no hardware graphics plug-in.
# Touch, buttons and the CPU clock table stay enabled; Wi-Fi, sound, camera,
# haptics and battery remain off (renoir/docs/ios26-boot.md).
BOOT_ARGS = ['haptic=0', 'cpufreq=1', 'gpu=0',
             'wifi=0', 'adsp=0', 'battmgr=0', 'speaker_probe=0', 'renoir_afe_probe=0', 'sound=0', 'cam=0',
             'cam_stream_test=0', 'touch_lat=0', 'touchscreen=1', 'buttons=1', 'launchd_unsecure_cache=1',
             # the panel's light (renoir/docs/backlight.md): 1 reads the DSI controller and logs it, nothing written
             # to it; --boot-arg backlight=2 gives the brightness slider and Auto-Brightness the panel
             'backlight=1',
             # the thermal limiter (renoir/docs/thermal.md): 1 logs the hottest sensor every ten seconds;
             # --boot-arg thermal=2 holds the processors and the panel back when hot
             'thermal=1']

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--wifi-mac', required=True, metavar='MAC', help="this handset's device Wi-Fi MAC")
p.add_argument('--kernel', type=Path,
               default=DK / 'xnu-12377-hakios/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837')
p.add_argument('--system', type=Path, default=IOS / 'system27-prune.apfs')
p.add_argument('--root', type=Path, default=IOS / 'root26-ios.apfs')
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


args = {a.split('=', 1)[0]: a for a in BOOT_ARGS}
for a in options.boot_arg:
    args[a.split('=', 1)[0]] = a
if options.return_after:
    args['loader_return_after'] = f'loader_return_after={options.return_after}'
loader = DR / 'prebuilt/renoir/resident-usb.img'
for path in (options.kernel, options.system, options.root, loader, DR / 'recovery/assets/renoir-ios-share.bin'):
    if not path.is_file():
        raise SystemExit(f'Missing {path} (renoir/docs/ios26-boot.md says how it is made)')
try:
    with options.system.open('rb') as image:
        validate_apfs(image.read(4096), options.system.stat().st_size)
    data = prepare((DR / 'recovery/assets/renoir-ios-share.bin').read_bytes(), time.time(),
                   wifi_mac=validate_mac(options.wifi_mac), kernel=options.kernel.read_bytes(), cpu_count=8,
                   ramdisk=options.root.read_bytes(), boot_args=list(args.values()))
except (OSError, ValueError) as error:
    raise SystemExit(str(error))

dev = devices()
if len(dev) != 1:
    raise SystemExit('Connect exactly one phone in fastboot mode')
if dev[0] == 'xnuloader':
    raise SystemExit('Restart the phone into stock fastboot first; resident RAM may be in use')
check(dev[0], 'product', 'renoir')
cmd('boot', file(loader), serial=dev[0])
for _ in range(60):
    if 'xnuloader' in devices():
        break
    time.sleep(1)
else:
    raise SystemExit('Resident loader did not appear; do not flash anything')
check('xnuloader', 'board', 'renoir')
cmd('oem', 'ramdisk', serial='xnuloader')
print(f'Uploading the iOS system image ({options.system.stat().st_size / 1e9:.2f} GB)...', flush=True)
cmd('stage', file(options.system), serial='xnuloader', timeout=900, live=True)
data = prepare(data, time.time())                       # the clock again, after the long upload
run = IOS / 'run26-current.bin'
run.write_bytes(data)
cmd('stage', file(run), serial='xnuloader', timeout=180, live=True)
cmd('oem', 'run', serial='xnuloader')
print('RAM run requested. Inspect the screen/log to verify startup. Nothing was flashed or erased.')
