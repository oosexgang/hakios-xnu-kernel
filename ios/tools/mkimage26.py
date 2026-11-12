#!/usr/bin/env python3
"""The iOS 26 system image (the RAM disk /dev/md0 the loader stages), arm64e code made runnable on a
phone's ARMv8.2 cores: every arm64e Mach-O goes through pacstrip, the dyld shared cache is the one
pacstrip rewrote (ios26/dyld-strip).

usage: [IOS=DIR] mkimage26.py [--min | --prune] OUT.apfs      (IOS: the extracted IPSW, default ios26)

  --min   only what proves the user space runs: launchd, dyld, the cache and a few programs of /bin
          and /usr/bin; the rest of the system is left out
  (default) the files darwin-renoir/ios165/select-system.py chooses, applied to iOS 26
  --prune the same, smaller for 6 GB of RAM (docs/ram-budget.md, scenario B): .loctable string
          tables cut to English and Base, the heavy apps nothing at boot needs and the poster apps
          left out, and Fonts/AppFonts

/sbin/launchd is darwin-renoir's stage0 (it mounts the small image on /hakios and runs its init), the
real one /sbin/launchd.ios. With LAUNCHDAEMONS=DIR (ios/userland/root26-ios.sh writes ios26/jobs26)
the jobs are in /System/Library/LaunchDaemons itself and in the launchd job cache
System/Library/xpc/launchd.plist; without it /System/Library/LaunchDaemons leads to /hakios/LaunchDaemons,
as on 16.5 (enough for the restore-mode boot of the first probes, not for a normal one).
"""
import importlib.util
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

DK = os.environ.get('DK') or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
I = os.environ.get('IOS', DK + '/ios26')         # the extracted IPSW: ios26 (23G90) or ios27 (24B119)
ROOT = I + '/rootfs'
CACHE = I + '/dyld-strip'
DR = DK + '/darwin-renoir'
HERE = os.path.dirname(os.path.abspath(__file__))
PACSTRIP = HERE + '/pacstrip'
WORK = os.environ.get("WORK", I + "/image-tree")
CACHE_DIR = 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
IMAGE_ROOT = 'hakios'                            # where stage0 mounts the small image
STAGE0 = DR + '/userland/obj/stage0'


def stage0_root(path=STAGE0):
    """The directory the stage0 at path mounts the small image on: the one it runs init from
    ("/NAME/sbin/init" in it). A stage0 built from an older tree may say another name than IMAGE_ROOT;
    the system volume then gives that name as a link to IMAGE_ROOT, so it mounts the image there and its
    paths lead there."""
    try:
        with open(path, 'rb') as f:
            found = re.search(rb'\x00/([A-Za-z0-9_.-]+)/sbin/init\x00', f.read())
    except OSError:
        return IMAGE_ROOT
    return found.group(1).decode() if found else IMAGE_ROOT

MIN_FILES = ['usr/lib/dyld', 'sbin/launchd', 'bin/ps', 'bin/df', 'usr/bin/uname', 'usr/bin/sw_vers',
             'usr/bin/id', 'usr/bin/true', 'usr/sbin/sysctl', 'usr/libexec/xpcproxy',
             'System/Library/CoreServices/SystemVersion.plist']


PRUNED_APPS = ('Podcasts', 'Maps', 'Freeform', 'Music', 'Books', 'Setup', 'Journal', 'Bridge', 'Fitness',
               'SOSBuddy', 'Diagnostics', 'DiagnosticsService', 'SequoiaTranslator', 'Games', 'AppleTV', 'News',
               'ContinuityCaptureShieldUI')
# What cannot work on this phone and is big (iOS 27's image is 4.59 GB with the rules above, over the
# 4 GiB - 1 that fastboot can send in one download): the RAW camera's neural networks, Metal portrait
# filters, the A13 camera ISP's firmware, DriverKit
# drivers (this kernel loads none), and frameworks whose code is in the shared cache while their resources
# serve hardware or services not here (Apple Intelligence, camera effects, iWork document previews, the App
# Store in Messages).
PRUNED_PATHS = ('System/Library/CoreServices/RawCamera.bundle/RawSFD/',
                'System/Library/CoreServices/RawCameraSupport.bundle/PlugIns.assets',
                'System/Library/Isp/', 'System/Library/DriverExtensions/',
                'System/Library/Messages/iMessageBalloons/ASMessagesProvider.bundle/')
PRUNED_METALLIB_DIRS = ('System/Library/CoreImage/PortraitFilters.cifilter/',)
PRUNED_FRAMEWORK_RESOURCES = ('System/Library/PrivateFrameworks/iWorkImport.framework/',
                              'System/Library/PrivateFrameworks/IntelligenceFlowPlannerSupport.framework/',
                              'System/Library/PrivateFrameworks/CameraEffectsKit.framework/')
EMOJI = 'System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc'
# What select-system.py (16.5's rules: framework resources up to SMALL) leaves out and iOS 27 cannot
# do without: linkd dies at its start without the App Intents schema database ("AppIntentSchemas/
# Database.swift:80: Fatal error: Failed to open database connection: databaseFileMissing"); and
# QuartzCore's shader resource stays beside its bundle, even with hardware graphics disabled.
KEPT_FILES = ('System/Library/PrivateFrameworks/AppIntentSchemas.framework/AppIntentSchemas.sqlite',
              'System/Library/Frameworks/QuartzCore.framework/default.metallib')
LOCTABLE_KEEP = ('en', 'Base', 'English', 'en_US', 'en_GB')


def pruned(rel):
    if rel.startswith('Applications/'):
        app = rel.split('/')[1]
        return app.endswith('PosterApp.app') or app[:-len('.app')] in PRUNED_APPS
    if rel.startswith(PRUNED_PATHS):
        return True
    if rel.startswith(PRUNED_METALLIB_DIRS) and rel.endswith('.metallib'):
        return True
    if rel.startswith(PRUNED_FRAMEWORK_RESOURCES) and not rel.endswith('/Info.plist'):
        return True
    return rel.startswith('System/Library/Fonts/AppFonts/')


def trim_emoji():
    """the emoji font with the strikes the screen uses only (darwin-renoir tools/font_trim.py, as 16.5)"""
    path = os.path.join(WORK, EMOJI)
    if not os.path.isfile(path):
        return
    tmp = path + '.trim'
    subprocess.run([sys.executable, DR + '/tools/font_trim.py', 'emoji', path, tmp], check=True)
    os.replace(tmp, path)       # the file in WORK is a hard link to the rootfs: replaced, not written through


def trim_loctable(src, dst):
    """a .loctable with only the English and Base localizations (and its Loc* keys); False if it is not one"""
    import plistlib
    try:
        with open(src, 'rb') as f:
            table = plistlib.load(f)
    except Exception:
        return False
    if not isinstance(table, dict):
        return False
    kept = {k: v for k, v in table.items() if k in LOCTABLE_KEEP or k.startswith('Loc')}
    with open(dst, 'wb') as f:
        plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)
    shutil.copystat(src, dst)
    return True


PRUNE = False


def is_arm64e_macho(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
    except OSError:
        return False
    if len(head) < 32:
        return False
    magic = struct.unpack_from('<I', head)[0]
    if magic == 0xfeedfacf:
        cputype, subtype = struct.unpack_from('<ii', head, 4)
        return cputype == 0x0100000c and (subtype & 0xff) == 2
    if struct.unpack_from('>I', head)[0] in (0xcafebabe, 0xcafebabf):
        count = struct.unpack_from('>I', head, 4)[0]
        big = struct.unpack_from('>I', head)[0] == 0xcafebabf
        size = 32 if big else 20
        # (as many entries as were read: a Java class file has this magic too, with its version where the count is)
        for i in range(min(count, 16, (len(head) - 8) // size)):
            cputype, subtype = struct.unpack_from('>ii', head, 8 + i * size)
            if cputype == 0x0100000c and (subtype & 0xff) == 2:
                return True
    return False


def selection(minimal):
    if minimal:
        chosen = [(rel, ROOT + '/' + rel) for rel in MIN_FILES if os.path.isfile(ROOT + '/' + rel)]
        for dp, dn, fns in os.walk(ROOT + '/private/etc'):     # the user database, and the rest of /etc
            for fn in fns:
                full = os.path.join(dp, fn)
                if os.path.isfile(full) and not os.path.islink(full):
                    chosen.append((full[len(ROOT) + 1:], full))
        return chosen
    spec = importlib.util.spec_from_file_location('sel', DR + '/ios165/select-system.py')
    sel = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sel)
    # A launchd job is not usable without its executable. Framework-hosted daemons and large
    # /usr programs otherwise fall through the 16.5 resource-size limits and leave clients waiting.
    kept_files = set(KEPT_FILES)
    jobs = os.environ.get('LAUNCHDAEMONS')
    if jobs:
        for name in sorted(os.listdir(jobs)):
            if not name.endswith('.plist'):
                continue
            with open(os.path.join(jobs, name), 'rb') as f:
                job = plistlib.load(f)
            program = job.get('Program') or (job.get('ProgramArguments') or [None])[0]
            if program and program.startswith('/') and not program.startswith((f'/{IMAGE_ROOT}/', f'/{stage0_root()}/')):
                kept_files.add(program[1:])
    chosen = []
    for dp, dn, fns in os.walk(ROOT):
        for fn in fns:
            full = os.path.join(dp, fn)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            rel = full[len(ROOT) + 1:]
            if rel.startswith(sel.STAGED_APPS):
                rel = 'Applications/' + rel[len(sel.STAGED_APPS):]
            elif rel.startswith('private/var/'):
                continue
            if sel.wanted_file(rel, os.path.getsize(full)) or rel in kept_files:
                chosen.append((rel, full))
    for sub, prefix, skip in (('os', 'System/Cryptexes/OS/', ('System/Library/Caches/com.apple.dyld/', 'System/DriverKit/')),
                              ('app', 'System/Cryptexes/App/', ())):
        base = f'{I}/cryptex/{sub}'
        for dp, dn, fns in os.walk(base):
            for fn in fns:
                full = os.path.join(dp, fn)
                if os.path.islink(full) or not os.path.isfile(full):
                    continue
                rel = full[len(base) + 1:]
                if not rel.startswith(skip):
                    chosen.append((prefix + rel, full))
    return chosen


def links(minimal):
    """the symbolic links of the root filesystem (all of them: they cost nothing)"""
    out = []
    for dp, dn, fns in os.walk(ROOT):
        for name in dn + fns:
            full = os.path.join(dp, name)
            if os.path.islink(full):
                rel = full[len(ROOT) + 1:]
                if minimal and '/' in rel and not rel.startswith(('usr/lib/', 'System/Library/Frameworks/')):
                    continue
                out.append((rel, os.readlink(full)))
    return out


def place(item):
    rel, src = item
    dst = os.path.join(WORK, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    if PRUNE and rel.endswith('.loctable') and trim_loctable(src, dst):
        return 0
    if is_arm64e_macho(src):
        r = subprocess.run([PACSTRIP, 'macho', src, dst], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f'pacstrip {rel}: {r.stderr.strip()}')
        shutil.copymode(src, dst)
        return 1
    shutil.copy2(src, dst)
    return 0


def default_wallpaper():
    """
    The wallpaper iOS gives a new device, one it can draw here. DefaultWallpapers~iphone.plist names
    7565 (collection "iOS 17"), whose files are for the iPhone 11's screen class (414w-896h@2x) only,
    and pictures in HEIC; the display here is 375w-812h@3x, WallpaperKit finds "no compatible
    wallpaper" and PosterBoard falls back to a Mercury poster, which draws with Metal: a black screen.
    Stripe Blue (7390) is drawn by CoreAnimation layers for any screen class. (The file in WORK is a
    hard link to the extracted rootfs: replaced, not written through.)
    """
    import plistlib

    path = os.path.join(WORK, 'Library/Wallpaper/DefaultWallpapers~iphone.plist')
    with open(path, 'rb') as f:
        defaults = plistlib.load(f)
    defaults['default'] = {'wallpaperIdentifier': 7390, 'collectionIdentifier': '2939274C-F444-4AA8-806E-6E3FAFBA79B1'}
    os.remove(path)
    with open(path, 'wb') as f:
        plistlib.dump(defaults, f)


def write_job_cache(jobs):
    """
    System/Library/xpc/launchd.plist with our jobs. On a normal (not restore) boot launchd takes its jobs
    from this cache, which the boot task launchd_cache_loader hands it (RequireSuccess: without the file
    the boot ends). The cache is Apple's, with its LaunchDaemons replaced by ours; the boot-arg
    launchd_unsecure_cache=/System/Library/xpc/launchd.plist has it taken without the .sig and AMFI's
    trust cache, which this kernel has not got.
    """
    with open(ROOT + '/System/Library/xpc/launchd.plist', 'rb') as f:
        cache = plistlib.load(f)
    daemons = {}
    for name in sorted(os.listdir(jobs)):
        if name.endswith('.plist'):
            with open(os.path.join(jobs, name), 'rb') as f:
                daemons['/System/Library/LaunchDaemons/' + name] = plistlib.load(f)
    cache['LaunchDaemons'] = daemons
    os.makedirs(os.path.join(WORK, 'System/Library/xpc'), exist_ok=True)
    with open(os.path.join(WORK, 'System/Library/xpc/launchd.plist'), 'wb') as f:
        plistlib.dump(cache, f, fmt=plistlib.FMT_BINARY)
    print(f'mkimage26: launchd job cache with {len(daemons)} jobs')


def main():
    global PRUNE
    minimal = '--min' in sys.argv
    PRUNE = '--prune' in sys.argv
    out = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(out) != 1:
        sys.exit(__doc__)
    if os.path.exists(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    for d in ('dev', IMAGE_ROOT, 'private/var', 'private/preboot', 'private/etc', 'private/tmp', 'System/Library',
              'sbin', 'usr/lib', CACHE_DIR):
        os.makedirs(os.path.join(WORK, d), exist_ok=True)
    if stage0_root() != IMAGE_ROOT:
        print(f'mkimage26: this stage0 mounts the small image on /{stage0_root()}: a link to /{IMAGE_ROOT} there')
        os.symlink(IMAGE_ROOT, os.path.join(WORK, stage0_root()))
    chosen = list(dict(selection(minimal)).items())      # (one source for each path: the last)
    if PRUNE:
        chosen = [(rel, src) for rel, src in chosen if not pruned(rel)]
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        stripped = sum(pool.map(place, chosen))
    # launchd: ours first (stage0), Apple's as launchd.ios
    os.replace(os.path.join(WORK, 'sbin/launchd'), os.path.join(WORK, 'sbin/launchd.ios'))
    shutil.copy2(STAGE0, os.path.join(WORK, 'sbin/launchd'))
    for rel, target in links(minimal):
        dst = os.path.join(WORK, rel)
        if os.path.lexists(dst) or rel.startswith('System/Library/LaunchDaemons'):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.symlink(target, dst)
    # the audio server's output device (root26-ios.sh): the HAL loads plug-ins from this directory only
    hal = os.path.join(WORK, 'System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver')
    if not os.path.lexists(hal):
        os.makedirs(os.path.dirname(hal), exist_ok=True)
        os.symlink('/hakios/lib/HakiosAudio.driver', hal)
    # the jobs are ours (/hakios/LaunchDaemons, from mkjobs.py), as on 16.5: Apple's own lists go
    for rel in ('System/Library/NanoLaunchDaemons', 'System/Library/LaunchAngels', 'System/Library/xpc/launchd.plist'):
        path = os.path.join(WORK, rel)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        elif os.path.lexists(path):
            os.remove(path)
    # LAUNCHDAEMONS=DIR: the jobs go into the system volume itself. iOS 26's launchd counts a job as a
    # system service (one that may claim an event stream: notifyd's com.apple.notifyd.matching) only when
    # its plist was loaded from there; through the link to /hakios (another volume) none of them is.
    ld = os.path.join(WORK, 'System/Library/LaunchDaemons')
    if os.path.isdir(ld) and not os.path.islink(ld):
        shutil.rmtree(ld)
    jobs = os.environ.get('LAUNCHDAEMONS')
    if jobs:
        shutil.copytree(jobs, ld)
        write_job_cache(jobs)
    elif not os.path.lexists(ld):
        os.symlink('/hakios/LaunchDaemons', ld)
    if not minimal:
        default_wallpaper()
    if PRUNE:
        trim_emoji()
    for name in sorted(os.listdir(CACHE)):
        if name.endswith(('.symbols', '.atlas')):
            continue
        os.link(os.path.join(CACHE, name), os.path.join(WORK, CACHE_DIR, name))
    print(f'mkimage26: {len(chosen)} files ({stripped} arm64e Mach-Os rewritten by pacstrip), cache from {CACHE}')
    subprocess.run([sys.executable, DR + '/tools/apfs/mkapfs.py', '-z', '-o', out[0], 'System=' + WORK], check=True)
    if os.path.getsize(out[0]) > 0xffffffff:
        raise SystemExit('System image exceeds fastboot\'s 4 GiB - 1 download limit')


if __name__ == '__main__':
    main()
