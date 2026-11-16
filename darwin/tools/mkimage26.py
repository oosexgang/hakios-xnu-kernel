#!/usr/bin/env python3
"""The Darwin 26 system image (the RAM disk /dev/md0 the loader stages), arm64e code made runnable on a
phone's ARMv8.2 cores: every arm64e Mach-O goes through pacstrip, the dyld shared cache is the one
pacstrip rewrote (darwin26/dyld-strip).

usage: [IOS=DIR] [WALLPAPER=COLOUR] [METAL=1] mkimage26.py [--min | --prune] [--metal] OUT.apfs      (IOS: the unpacked IPSW, default ios27 beside the
                                                              repository; darwin/tools/unpack.sh)

  --min   only what proves the user space runs: launchd, dyld, the cache and a few programs of /bin
          and /usr/bin; the rest of the system is left out
  (default) the files select-system.py chooses (the rules of 16.5's image), applied to iOS 26/27
  --prune the same, smaller for 6 GB of RAM (docs/ram-budget.md, scenario B): .loctable string
          tables cut to English and Base, the heavy apps nothing at boot needs and the poster apps
          left out, and Fonts/AppFonts
  --metal with the system's Metal libraries (.metallib), which select-system.py leaves out, for a boot
          with metal=1: CoreAnimation, Core Image and Metal Performance Shaders read their shaders from
          them (the machine-learning ones excepted: METAL_LEFT_OUT). About 0.3 GB more: an image for the
          flash (darwin/tools/darwin-write.py), past what one fastboot download takes into RAM

/System/Library/Extensions/HakiosMetal.bundle leads to /hakios/lib/HakiosMetal.bundle, the plug-in Metal opens
when the kernel names it (boot-arg metal=1; metal/plugin, built by darwin/userland/metal.sh into the small image).

/sbin/launchd is our stage0 ($OBJ/stage0, which darwin/userland/root26-darwin.sh builds: it mounts the small image
on /hakios and runs its init), the real one /sbin/launchd.darwin. With LAUNCHDAEMONS=DIR (darwin/userland/root26-darwin.sh writes $IOS/jobs26)
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

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
I = os.environ.get('IOS') or os.environ.get('DARWIN_FW') or os.path.join(os.path.dirname(REPO), 'ios27')  # the unpacked IPSW
ROOT = I + '/rootfs'
CACHE = I + '/dyld-strip'
OBJ = os.environ.get('OBJ') or I + '/obj'           # what darwin/userland/root26-darwin.sh built: stage0
APFS = REPO + '/userland/tools/apfs/mkapfs.py'
PACSTRIP = HERE + '/pacstrip'
WORK = os.environ.get("WORK", I + "/image-tree")
CACHE_DIR = 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
IMAGE_ROOT = 'hakios'                            # where stage0 mounts the small image
STAGE0 = os.environ.get('STAGE0') or OBJ + '/stage0'


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
# What cannot work on this phone and is big (Darwin 27's image is 4.59 GB with the rules above, over the
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
# What select-system.py (16.5's rules: framework resources up to SMALL) leaves out and Darwin 27 cannot
# do without: linkd dies at its start without the App Intents schema database ("AppIntentSchemas/
# Database.swift:80: Fatal error: Failed to open database connection: databaseFileMissing"); and
# QuartzCore's shader resource stays beside its bundle, even with hardware graphics disabled.
KEPT_FILES = ('System/Library/PrivateFrameworks/AppIntentSchemas.framework/AppIntentSchemas.sqlite',
              'System/Library/Frameworks/QuartzCore.framework/default.metallib')
LOCTABLE_KEEP = ('en', 'Base', 'English', 'en_US', 'en_GB')
# METAL=1: what the Metal driver (userland/src/metal_driver.m) needs that the rules above leave out. SpringBoard
# makes an MPSImageGaussianBlur (Siri's orb) as soon as Metal has a device, and MetalPerformanceShaders asserts
# without its kernels' libraries ("metallib not found: default.metallib for identifier
# com.apple.MetalPerformanceShaders.MPSImage", Darwin 27 under QEMU). 56 MB before compression, with RenderBox's.
# RenderBox (SwiftUI's renderer, in SpringBoard and iconservicesagent among others) draws with Metal as well and
# aborts without its own shaders (Darwin 27 under QEMU, once MPS had its libraries).
MPS = 'System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/'
METAL_FILES = tuple(MPS + f'{name}.framework/default.metallib' for name in ('MPSCore', 'MPSImage', 'MPSFunctions')) + \
    tuple(f'System/Library/PrivateFrameworks/RenderBox.framework/{name}.metallib' for name in ('default', 'archive'))
# --metal: the Metal libraries that stay out all the same, 0.26 GB of machine learning's and the GPU tools' that
# nothing on the screen draws with (GESS's MLX, Espresso, MPS's graph and neural network kernels, the GPU trace
# replayer)
METAL_LEFT_OUT = ('System/Library/PrivateFrameworks/GESS.framework/', 'System/Library/PrivateFrameworks/Espresso.framework/',
                  'System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSNDArray.framework/',
                  'System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSNeuralNetwork.framework/',
                  'System/Library/PrivateFrameworks/GPUToolsReplay.framework/')
METAL = False


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
    """the emoji font with the strikes the screen uses only (userland/tools/font_trim.py, as 16.5)"""
    path = os.path.join(WORK, EMOJI)
    if not os.path.isfile(path):
        return
    tmp = path + '.trim'
    subprocess.run([sys.executable, REPO + '/userland/tools/font_trim.py', 'emoji', path, tmp], check=True)
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
    spec = importlib.util.spec_from_file_location('sel', HERE + '/select-system.py')
    sel = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sel)
    # A launchd job is not usable without its executable. Framework-hosted daemons and large
    # /usr programs otherwise fall through the 16.5 resource-size limits and leave clients waiting.
    kept_files = set(KEPT_FILES)
    if os.environ.get('METAL'):
        kept_files.update(METAL_FILES)
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
            if sel.wanted_file(rel, os.path.getsize(full)) or rel in kept_files or \
                    (METAL and rel.endswith('.metallib') and not rel.startswith(METAL_LEFT_OUT)):
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


def english_lprojs():
    """
    The English LANGUAGE.lproj directories of the bundles that went in, made where they are missing. Since iOS
    16 most bundles keep every language's strings in one .loctable file, and the .lproj directories beside it are
    empty: they are only how CoreFoundation knows which languages the bundle has. A walk of files never meets
    an empty directory, so they were not in the image, and every string of such a bundle came out as its key --
    SpringBoard's low battery alert said LOW_BATT_TITLE, ENABLE_LOW_POWER and CLOSE (select-system.py made
    them for 16.5's image: KEEP_LANG).
    """
    keep = ('en', 'Base', 'English', 'en_US', 'en_GB')
    made = 0
    for dp, dn, fns in os.walk(ROOT):
        for name in dn:
            if not name.endswith('.lproj') or name[:-len('.lproj')] not in keep:
                continue
            full = os.path.join(dp, name)
            if os.path.islink(full):
                continue
            rel = full[len(ROOT) + 1:]
            if rel.startswith('private/var/staged_system_apps/'):
                rel = 'Applications/' + rel[len('private/var/staged_system_apps/'):]
            elif rel.startswith('private/var/'):
                continue
            bundle = os.path.join(WORK, os.path.dirname(rel))
            dst = os.path.join(WORK, rel)
            if os.path.isdir(bundle) and not os.path.lexists(dst):
                os.makedirs(dst)
                made += 1
    return made


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
    # A hard link where it can be one, as the cache's files are (main()): the tree is a few GB of files the
    # image builder only reads, and the edits below (trim_emoji, default_wallpaper) replace a file rather
    # than write through it. A copy where the rootfs is on another file system.
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)
    return 0


# The Stripe collection: the colours there are, each drawn by CoreAnimation layers for any screen class
STRIPE_COLLECTION = '2939274C-F444-4AA8-806E-6E3FAFBA79B1'
STRIPE = {'green': 7365, 'yellow': 7370, 'orange': 7375, 'red': 7380, 'purple': 7385, 'blue': 7390, 'black': 7395}


def wallpaper_identifier(name):
    """the Stripe wallpaper named NAME (a colour of STRIPE, or its number)"""
    name = name.strip().lower()
    if name in STRIPE:
        return STRIPE[name]
    if name.isdigit() and int(name) in STRIPE.values():
        return int(name)
    raise SystemExit(f"WALLPAPER={name}: one of {', '.join(STRIPE)}")


def default_wallpaper():
    """
    The wallpaper Darwin gives a new device, one it can draw here. DefaultWallpapers~iphone.plist names
    7565 (collection "Darwin 17"), whose files are for the iPhone 11's screen class (414w-896h@2x) only,
    and pictures in HEIC; the display here is 375w-812h@3x, WallpaperKit finds "no compatible
    wallpaper" and PosterBoard falls back to a Mercury poster, which draws with Metal: a black screen.
    The Stripe wallpapers are drawn by CoreAnimation layers for any screen class: Stripe Yellow (7370),
    or the colour WALLPAPER= names (green, yellow, orange, red, purple, blue, black). (The file in WORK
    is a hard link to the extracted rootfs: replaced, not written through.)
    """
    import plistlib

    identifier = wallpaper_identifier(os.environ.get('WALLPAPER', 'yellow'))
    path = os.path.join(WORK, 'Library/Wallpaper/DefaultWallpapers~iphone.plist')
    with open(path, 'rb') as f:
        defaults = plistlib.load(f)
    defaults['default'] = {'wallpaperIdentifier': identifier, 'collectionIdentifier': STRIPE_COLLECTION}
    os.remove(path)
    with open(path, 'wb') as f:
        plistlib.dump(defaults, f)
    print(f'mkimage26: the default wallpaper is Stripe {identifier}')


JB = os.environ.get('JB') or REPO + '/userland/jb/root/var/jb'
# what Sileo moves, copies and removes files with as root, from /bin (userland/build.sh's SILEO_BIN)
SILEO_BIN = ('chmod', 'cp', 'mkdir', 'mv', 'rm')


def package_prefix():
    """
    The package manager's prefix, when userland/tools/mk-jb.sh has fetched one (JB=DIR for another, JB=0 for none):
    the Procursus bootstrap with Sileo and ElleKit in it, as /jb, which init grafts onto /private/var/jb at every
    boot (userland/userland/src/init.c; var_store leaves it out). Its applications are system applications here, in
    /Applications, where LaunchServices finds them by itself; and /bin gets the five programs Sileo runs from
    there as root, which iOS's /bin (df and ps) has not: without them every package Sileo downloaded stopped
    short of apt's archives ("Unable to fetch some archives"), as on 16.5.
    """
    if os.environ.get('JB') == '0' or not os.path.isdir(JB):
        return
    import glob

    dst = os.path.join(WORK, 'jb')
    shutil.copytree(JB, dst, symlinks=True)
    apps = []
    for app in sorted(glob.glob(os.path.join(dst, 'Applications', '*.app'))):
        shutil.copytree(app, os.path.join(WORK, 'Applications', os.path.basename(app)), symlinks=True)
        apps.append(os.path.basename(app))
    os.makedirs(os.path.join(WORK, 'bin'), exist_ok=True)
    for name in SILEO_BIN:
        link = os.path.join(WORK, 'bin', name)
        if not os.path.lexists(link):
            os.symlink('/var/jb/usr/bin/' + name, link)
    files = sum(len(f) + len(d) for _, d, f in os.walk(dst))
    print(f'mkimage26: the package manager\'s prefix as /jb ({files} entries), {", ".join(apps) or "no applications"} '
          f'in /Applications, /bin/{{{",".join(SILEO_BIN)}}} into it')


def modem_firmware():
    """
    MODEM_FW=DIR: the phone's modem firmware as /fw of the image, where the modem driver (boot-arg modem=1,
    kernel/overlay/bsd/dev/hakios_mpss.c) reads it: DIR/image is the phone's /vendor/firmware_mnt/image (with
    /vendor/firmware's yupik_ipa_fws.* put in it), served to the modem over TFTP as /fw/image, and the modem's own
    image, modem.mdt and its segments, is /fw/modem.* as well (85 MB: too much for the small image). The phone's
    own files (firmware/BOARD/, never published).
    """
    src = os.environ.get('MODEM_FW')
    if not src:
        return
    image = os.path.join(src, 'image')
    if not os.path.isfile(os.path.join(image, 'modem.mdt')):
        raise SystemExit(f'mkimage26: MODEM_FW={src} has no image/modem.mdt (the phone\'s /vendor/firmware_mnt/image)')
    dst = os.path.join(WORK, 'fw')
    shutil.copytree(image, os.path.join(dst, 'image'))
    for name in sorted(os.listdir(image)):
        if name.startswith('modem.'):
            shutil.copy2(os.path.join(image, name), os.path.join(dst, name))
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(dst) for f in fs)
    print(f'mkimage26: the modem\'s firmware as /fw ({size / 1e6:.0f} MB)')


def angels_placed():
    """
    The names of the jobs that are launch angels (System/Library/LaunchAngels of the IPSW), which go back to that
    directory of the image instead of LaunchDaemons, and into the job cache by that directory's path (write_job_cache).
    launchd starts an angel in the user's domain (osservice<com.apple.PosterBoard(501)> on an iPhone); as a system
    daemon PosterBoard was a process FrontBoard did not know, and the lock screen's poster switcher, an application
    scene of it, ended SpringBoard ("process was nil" in -[SBApplicationSceneUpdateTransaction
    _notifyObserversOfClientConnection:]). LAUNCHANGELS=0: none, every job a daemon as before.
    """
    if os.environ.get('LAUNCHANGELS', '1') != '1':
        return set()
    src = ROOT + '/System/Library/LaunchAngels'
    return {name for name in os.listdir(src) if name.endswith('.plist')} if os.path.isdir(src) else set()


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
    angels = angels_placed()
    for name in sorted(os.listdir(jobs)):
        if name.endswith('.plist'):
            # (an angel is in the same dictionary, by its own directory's path: that path is what launchd
            # knows an angel by, as Apple's cache has com.apple.PosterBoard; left out, it was no job at all,
            # and PosterBoard never ran)
            where = 'LaunchAngels' if name in angels else 'LaunchDaemons'
            with open(os.path.join(jobs, name), 'rb') as f:
                daemons[f'/System/Library/{where}/' + name] = plistlib.load(f)
    cache['LaunchDaemons'] = daemons
    os.makedirs(os.path.join(WORK, 'System/Library/xpc'), exist_ok=True)
    with open(os.path.join(WORK, 'System/Library/xpc/launchd.plist'), 'wb') as f:
        plistlib.dump(cache, f, fmt=plistlib.FMT_BINARY)
    print(f'mkimage26: launchd job cache with {len(daemons)} jobs')


def main():
    global PRUNE, METAL
    minimal = '--min' in sys.argv
    PRUNE = '--prune' in sys.argv
    METAL = '--metal' in sys.argv
    out = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(out) != 1:
        sys.exit(__doc__)
    wallpaper_identifier(os.environ.get('WALLPAPER', 'yellow'))     # (a wrong name stops the build here, not after it)
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
    lprojs = english_lprojs()
    # launchd: ours first (stage0), Apple's as launchd.darwin
    os.replace(os.path.join(WORK, 'sbin/launchd'), os.path.join(WORK, 'sbin/launchd.darwin'))
    shutil.copy2(STAGE0, os.path.join(WORK, 'sbin/launchd'))
    for rel, target in links(minimal):
        dst = os.path.join(WORK, rel)
        if os.path.lexists(dst) or rel.startswith('System/Library/LaunchDaemons'):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.symlink(target, dst)
    # the audio server's output device (root26-darwin.sh): the HAL loads plug-ins from this directory only
    hal = os.path.join(WORK, 'System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver')
    if not os.path.lexists(hal):
        os.makedirs(os.path.dirname(hal), exist_ok=True)
        os.symlink('/hakios/lib/HakiosAudio.driver', hal)
    # the Metal plug-in: Metal opens /System/Library/Extensions/<the accelerator's MetalPluginName>.bundle
    plugin = os.path.join(WORK, 'System/Library/Extensions/HakiosMetal.bundle')
    if not os.path.lexists(plugin):
        os.makedirs(os.path.dirname(plugin), exist_ok=True)
        os.symlink('/hakios/lib/HakiosMetal.bundle', plugin)
    # the jobs are ours (/hakios/LaunchDaemons, from mkjobs.py), as on 16.5: Apple's own lists go
    for rel in ('System/Library/NanoLaunchDaemons', 'System/Library/LaunchAngels', 'System/Library/xpc/launchd.plist'):
        path = os.path.join(WORK, rel)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        elif os.path.lexists(path):
            os.remove(path)
    # LAUNCHDAEMONS=DIR: the jobs go into the system volume itself. Darwin 26's launchd counts a job as a
    # system service (one that may claim an event stream: notifyd's com.apple.notifyd.matching) only when
    # its plist was loaded from there; through the link to /hakios (another volume) none of them is.
    ld = os.path.join(WORK, 'System/Library/LaunchDaemons')
    if os.path.isdir(ld) and not os.path.islink(ld):
        shutil.rmtree(ld)
    jobs = os.environ.get('LAUNCHDAEMONS')
    if jobs:
        shutil.copytree(jobs, ld)
        angels = angels_placed()
        if angels:
            la = os.path.join(WORK, 'System/Library/LaunchAngels')
            os.makedirs(la, exist_ok=True)
            moved = 0
            for name in sorted(angels):
                if os.path.exists(os.path.join(ld, name)):
                    os.replace(os.path.join(ld, name), os.path.join(la, name))
                    moved += 1
            print(f'mkimage26: {moved} launch angels in System/Library/LaunchAngels')
        write_job_cache(jobs)
    elif not os.path.lexists(ld):
        os.symlink('/hakios/LaunchDaemons', ld)
    if not minimal:
        default_wallpaper()
    if PRUNE:
        trim_emoji()
    if not minimal:
        package_prefix()
        modem_firmware()
    for name in sorted(os.listdir(CACHE)):
        if name.endswith(('.symbols', '.atlas')):
            continue
        os.link(os.path.join(CACHE, name), os.path.join(WORK, CACHE_DIR, name))
    print(f'mkimage26: {len(chosen)} files ({stripped} arm64e Mach-Os rewritten by pacstrip), {lprojs} empty English '
          f'.lproj directories, cache from {CACHE}')
    subprocess.run([sys.executable, APFS, '-z', '-o', out[0], 'System=' + WORK], check=True)
    if os.path.getsize(out[0]) > 0xffffffff:
        if not METAL:
            raise SystemExit('System image exceeds fastboot\'s 4 GiB - 1 download limit')
        # (--metal: an image for the flash, which has no such limit)
        print(f'mkimage26: {os.path.getsize(out[0]) / 1e9:.2f} GB: for the flash only (darwin/tools/darwin-write.py), '
              'past what one fastboot download takes into RAM')


if __name__ == '__main__':
    main()
