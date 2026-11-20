#!/usr/bin/env python3
"""Before a day with the phone: is everything here that its runs need? What ./bench and the runs of
Documentation/phone-test.ru.md take from this machine, looked at before the phone is connected rather than found
missing in the middle of the day: the phone's package, adb and fastboot, the kernel and the loader (built, and not older
than what they are built from), both knowing the phone, which iPhone Darwin is told it is, the small image, and
what a run asks for beyond them (the Bluetooth controller's firmware at bt=2, the radio's address at wifi=).

  scripts/phone-ready.py [--board NAME] [RUN...]

RUN: defconfigs of arch/arm64/configs, those scripts/phone-day.sh runs when none is named. The phone is BENCH_BOARD
(or HAKIOS_BOARD, or --board), else the only package here with a uefi { } node, as for ./bench; BENCH_KERNEL, XNU_SRC,
BENCH_IDENTITY, BENCH_RAMDISK and IOS count as they do for ./bench.

Each line is one thing: ok, OLD (there, but older than what it is made from: build it again), NOTE (a run will go
without it, and do less) or MISSING (./bench would stop). Exit 0 when nothing is MISSING, 1 when something is, 2 for
a run or a phone that is not here.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import boards  # noqa: E402  (arch/arm64/boot/dts/NAME/board.dts)
import defconfig  # noqa: E402  (arch/arm64/configs)

RUNS = ('phone_first', 'phone_look', 'phone_ufs_check', 'phone_perf', 'phone_metal', 'phone_bt')
# what the kernel is built from (tools/kernel-view.sh puts them into the tree), and the loader
KERNEL_SOURCES = ('drivers', 'include', 'kernel', 'security', 'fs', 'crypto', 'lib', 'init', 'arch/arm64/kernel')
LOADER_SOURCES = ('arch/arm64/boot/src', 'arch/arm64/boot/build.sh')
PLACEHOLDER = b'hakios-placeholder'     # what a loader built without an iPhone's identity carries (tools/image.py)


class Report:
    def __init__(self):
        self.lines = []

    def add(self, state, what, text):
        self.lines.append((state, what, text))

    def missing(self):
        return any(state == 'MISSING' for state, _, _ in self.lines)

    def text(self):
        width = max((len(what) for _, what, _ in self.lines), default=0)
        return '\n'.join(f'  {state:<8} {what:<{width}}  {text}' for state, what, text in self.lines)


def when(seconds):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(seconds))


def last_change(paths):
    """(time, what) of the newest change to PATHS: a file changed and not committed, else the last commit to them or
    the newest file of them as it is here -- a pull or a checkout brings commits made before the build with the time
    they arrive, which is what a file's time says; None outside a git checkout."""
    def git(*arguments):
        try:
            done = subprocess.run(['git', '-C', str(ROOT), *arguments], capture_output=True, text=True)
        except OSError:
            return None                 # (no git here: nothing to compare with)
        return done.stdout if done.returncode == 0 else None
    changed = git('status', '--porcelain', '--', *paths)
    if changed is None:
        return None
    newest = None
    for line in changed.splitlines():
        path = ROOT / line[3:].split(' -> ')[-1]
        if path.is_file() and (newest is None or path.stat().st_mtime > newest[0]):
            newest = (path.stat().st_mtime, f'{path.relative_to(ROOT)}, not committed')
    if newest:
        return newest
    for name in (git('ls-files', '-z', '--', *paths) or '').split('\0'):
        path = ROOT / name
        if name and path.is_file() and (newest is None or path.stat().st_mtime > newest[0]):
            newest = (path.stat().st_mtime, f'{name} as it is here (pulled or checked out then)')
    commit = (git('log', '-1', '--format=%ct %h %s', '--', *paths) or '').strip()
    if commit:
        stamp, rest = commit.split(' ', 1)
        if newest is None or int(stamp) >= newest[0]:
            return int(stamp), f'commit {rest}'
    return newest


def current(report, what, path, sources, how):
    """ok, or OLD when PATH was built before the newest change to SOURCES."""
    built = path.stat().st_mtime
    change = last_change(sources)
    if change and change[0] > built:
        report.add('OLD', what, f'{path}, built {when(built)}, before {change[1]} ({when(change[0])}): {how}')
        return
    report.add('ok', what, f'{path}, built {when(built)}')


def board_named(name):
    """(name, its board) of the phone: NAME, or the only package here with a uefi { } node."""
    phones = {n: b for n, b in boards.load()[1].items() if 'uefi' in b}
    if name is None:
        if len(phones) != 1:
            raise SystemExit('phone-ready.py: which phone? BENCH_BOARD=NAME or --board NAME (here: ' +
                             (', '.join(sorted(phones)) or 'no package with a uefi { } node; tools/device.py add NAME') + ')')
        name = next(iter(phones))
    if name not in phones:
        raise SystemExit(f'phone-ready.py: no phone "{name}" here, a package with a uefi {{ }} node (tools/boards.py list)')
    return name, phones[name]


def check(name, board, runs, env):
    report = Report()
    xnu_src = Path(env.get('XNU_SRC') or ROOT.parent / 'xnu-12377-hakios')
    ios = Path(env.get('IOS') or ROOT.parent / 'ios27')

    # adb and fastboot: ./bench speaks to the phone through them
    for tool in ('fastboot', 'adb'):
        found = shutil.which(tool, path=env.get('PATH', ''))
        report.add('ok' if found else 'MISSING', tool, found or 'not on PATH: scripts/setup-ubuntu.sh phone '
                   '(android-sdk-platform-tools)')

    # the phone's UEFI build: ./bench enter makes the boot image the resident loader starts from
    uefi = ROOT / 'firmware' / name / 'uefi'
    report.add('ok' if uefi.is_dir() and any(uefi.iterdir()) else 'MISSING', 'uefi',
               f'{uefi}' if uefi.is_dir() and any(uefi.iterdir()) else
               f'nothing in {uefi}: the phone\'s UEFI build (Documentation/porting.md, step 2)')

    kernel = Path(env.get('BENCH_KERNEL') or xnu_src / 'BUILD/obj/mach.development.bcm2837')
    if not kernel.is_file():
        report.add('MISSING', 'kernel', f'no {kernel}: kernel/build.sh (make kernel)')
    elif kernel.read_bytes()[:4] != b'\xcf\xfa\xed\xfe':
        report.add('MISSING', 'kernel', f'{kernel} is not a 64-bit Mach-O kernel')
    elif b'\0' + name.encode() + b'\0' not in kernel.read_bytes():
        # (the board table is built into the kernel: one that does not know the phone finds no board, every driver off)
        report.add('MISSING', 'kernel', f'{kernel} does not know {name}: it was built before the package was here. '
                   'kernel/build.sh (make kernel)')
    elif env.get('BENCH_KERNEL'):
        report.add('ok', 'kernel', f'{kernel} (BENCH_KERNEL: whether it is current is yours to know)')
    else:
        current(report, 'kernel', kernel, KERNEL_SOURCES, 'kernel/build.sh (make kernel)')

    loader_path = ROOT / 'arch/arm64/boot/loader.efi'
    loader = loader_path.read_bytes() if loader_path.is_file() else b''
    if loader[:2] != b'MZ':
        report.add('MISSING', 'loader', f'no {loader_path}: arch/arm64/boot/build.sh (make loader)' if not loader else
                   f'{loader_path} is not a UEFI application: arch/arm64/boot/build.sh')
    elif b'\0' + name.encode() + b'\0' not in loader:
        report.add('MISSING', 'loader', f'{loader_path} does not know {name}: it was built before the package was here. '
                   'arch/arm64/boot/build.sh (make loader), then ./bench reload')
    else:
        current(report, 'loader', loader_path, LOADER_SOURCES, 'arch/arm64/boot/build.sh (make loader), then ./bench reload')

    # which iPhone Darwin is told it is: built into the loader, or given with each run
    identity = env.get('BENCH_IDENTITY') or str(ROOT / 'firmware' / name / 'identity.bin')
    if loader and PLACEHOLDER not in loader:
        report.add('ok', 'identity', 'built into the loader (arch/arm64/boot/identity.h, from an IPSW)')
    elif Path(identity).is_file() and Path(identity).stat().st_size > 0:
        report.add('ok', 'identity', identity + (' (BENCH_IDENTITY)' if env.get('BENCH_IDENTITY') else ''))
    elif loader:
        report.add('MISSING', 'identity', f'none: the loader was built without one, and there is no {identity}. The '
                   'DeviceTree.bin darwin/tools/unpack.sh leaves beside your IPSW, put there or named by BENCH_IDENTITY')

    # the small image every run sends with the kernel, and the system image ./bench ramdisk sends once
    ramdisk = env.get('BENCH_RAMDISK')
    if ramdisk == 'none':
        report.add('NOTE', 'small image', 'none (BENCH_RAMDISK=none): the runs have no /hakios')
    else:
        small = Path(ramdisk) if ramdisk else next((p for p in (ios / 'root26-darwin.apfs', ios / 'root.apfs')
                                                     if p.is_file()), ios / 'root26-darwin.apfs')
        report.add('ok' if small.is_file() else 'MISSING', 'small image', f'{small}' if small.is_file() else
                   f'no {small}: darwin/userland/root26-darwin.sh (BENCH_RAMDISK, IOS)')
    system = ios / 'system.apfs'
    report.add('ok' if system.is_file() else 'NOTE', 'system image', f'{system}' if system.is_file() else
               f'no {system}: enough when the phone still holds the one ./bench ramdisk sent (./bench state says)')

    # what a run asks for beyond these
    bt = board.get('bluetooth', {})
    for run in runs:
        arguments = defconfig.arguments(run)
        if arguments.get('bt', '').split('=', 1)[-1] == '2' and 'rampatch' in bt and \
                arguments.get('bt_fw', 'bt_fw=1').split('=', 1)[-1] != '0' and bt.get('firmware', 'fw') == 'fw':
            files = [bt['rampatch']] + ([bt['nvm']] if 'nvm' in bt else [])
            directory = ROOT / 'userland/fw-bt'
            absent = [f for f in files if not (directory / f).is_file()]
            report.add('NOTE' if absent else 'ok', f'{run}: bt firmware',
                       f'no {", ".join(absent)} in {directory}: adb pull /vendor/bt_firmware/image/ userland/fw-bt/, then '
                       'the small image again (bt_fw=0: the controller on its ROM)' if absent else
                       f'{", ".join(files)} in {directory} (in the small image once it is made again after them)')
        if any(k == 'wifi' and v.split('=', 1)[-1] not in ('', '0') for k, v in arguments.items()):
            mac = ROOT / 'firmware' / name / 'wifi-mac'
            report.add('ok' if mac.is_file() else 'NOTE', f'{run}: wifi-mac', f'{mac}' if mac.is_file() else
                       f'no {mac}: the radio runs with the address its firmware makes up (one line, aa:bb:cc:dd:ee:ff)')
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(prog='scripts/phone-ready.py', description='Is everything here for a day with the phone?')
    parser.add_argument('--board')
    parser.add_argument('runs', nargs='*', metavar='RUN')
    options = parser.parse_args(argv)
    env = dict(os.environ)
    runs = options.runs or list(RUNS)
    for run in runs:
        try:
            defconfig.arguments(run)
        except SystemExit as stop:
            print(stop.code if isinstance(stop.code, str) else f'phone-ready.py: no run {run}', file=sys.stderr)
            return 2
    try:
        name, board = board_named(options.board or env.get('BENCH_BOARD') or env.get('HAKIOS_BOARD') or None)
    except SystemExit as stop:
        print(stop.code, file=sys.stderr)
        return 2
    report = check(name, board, runs, env)
    print(f'phone-ready: {name}, for {" ".join(runs)}')
    print(report.text())
    if report.missing():
        print('phone-ready: not ready: what is MISSING above stops ./bench')
        return 1
    print('phone-ready: ready' + (' (OLD: build it again first, or the phone runs what was here before)'
                                  if any(state == 'OLD' for state, _, _ in report.lines) else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
