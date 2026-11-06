#!/usr/bin/env python3
"""Copy stock iOS launchd job definitions into the small image, adjusted for the bring-up.

usage: mkjobs.py [--rest] IOS_ROOTFS DEST_DIR label...

For every label, IOS_ROOTFS/System/Library/LaunchDaemons/<label>.plist is copied to DEST_DIR with
its standard error sent to the console, which is where a daemon's last words are read (there
is no log viewer or crash reporter yet). For the jobs named in the environment variable OSLOG
("all" for every one) OS_ACTIVITY_DT_MODE is set too, which makes os_log print there as well:
everything the daemon has to say -- SpringBoard alone says a megabyte in half a minute, which
costs time and pushes everybody else out of the log that comes back from the phone.
The program itself must be in the system image; a warning is printed if it was not extracted.

With --rest, the other jobs of iOS follow (their standard error on the console as well, their os_log
only when named in OSLOG), and with them the "launch angels" (System/Library/LaunchAngels: the
applications without an icon that draw parts of the system's own interface in a process of their
own -- the wallpaper, the keyboard, the now playing controls -- and are reached through a Mach
service like a daemon), but strictly on demand: whatever starts them by
itself (RunAtLoad, KeepAlive) is taken out, so that launchd runs one only when a program asks
for one of its Mach services. That is how an application finds the daemons it needs without
all 490 of them being started at boot. Left out: jobs without Mach services (nothing could
ever start them), jobs whose program is not in the system image, and those in DENY, which
drive hardware that is not there or do things nobody wants here.
"""
import os
import plistlib
import sys

# substrings of labels. (nsurlsessiond is not among them either, for all that it belongs to the same
# family of Apple's own services: it is the daemon that carries out background URLSession downloads,
# and without it an application's download never starts and never fails -- Sileo sits on "Queued"
# for ever, see «Sileo» in README.md.)
# (mediaserverd is not among them, for all that there is neither sound nor a camera:
# it is where VideoToolbox decodes for every other process, a HEIC picture included; see src/inject_media.c.)
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties', 'mobileactivationd',
        'installcoordinationd', 'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'apsd', 'itunesstored', 'appstored', 'cloudd', 'bird',
        # these start, crash and are asked for again, over and over. (aggregated, which was among them, is where
        # iOS 16 keeps its power log -- com.apple.powerlog.plxpclogger.xpc and the rest -- that Settings > Battery
        # waits for; it now runs through a session.)
        'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'avconferenced')

# ALLOW="word ..." in the environment lets jobs through that DENY would stop: for trying one out
DENY = tuple(word for word in DENY if word not in os.environ.get('ALLOW', '').split())

args = sys.argv[1:]
rest = args[:1] == ['--rest']
if rest:
    args = args[1:]
src_root, dest, labels = args[0], args[1], args[2:]
oslog = os.environ.get('OSLOG', '').split()
src = f'{src_root}/System/Library/LaunchDaemons'
# A program in a cryptex -- on a device a volume of its own, grafted at /System/Cryptexes/NAME (eight of
# Safari's daemons are in the App cryptex) -- is looked for where fetch.sh unpacked that cryptex, beside
# the root filesystem; the images have it at the same place (build.sh).
CRYPTEXES = {'/System/Cryptexes/OS/': 'cryptex', '/System/Cryptexes/App/': 'cryptex-app'}


def present(program):
    for prefix, unpacked in CRYPTEXES.items():
        if program.startswith(prefix):
            return os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(src_root)), unpacked,
                                               program[len(prefix):]))
    return os.path.isfile(f'{src_root}{program}')


angels = f'{src_root}/System/Library/LaunchAngels'
os.makedirs(dest, exist_ok=True)
for label in labels:
    # a launch angel named among them (build.sh, ANGELS) is taken from LaunchAngels, and like the rest's
    # started only when its Mach service is asked for
    angel = not os.path.isfile(f'{src}/{label}.plist') and os.path.isfile(f'{angels}/{label}.plist')
    with open(f'{angels if angel else src}/{label}.plist', 'rb') as f:
        job = plistlib.load(f)
    if angel:
        for key in ('RunAtLoad', 'KeepAlive'):
            job.pop(key, None)
    # TRIM="label:service,service;...": only those Mach services of the job, the rest left to fail at once as
    # if it were not there (mediaserverd for decoding HEIC alone: SpringBoard's synchronous request to its
    # system controller, FigSystemControllerRemoteCreate, is never answered on the Infinix -- run 253)
    for entry in filter(None, os.environ.get('TRIM', '').split(';')):
        trim_label, _, services = entry.partition(':')
        if trim_label == label and 'MachServices' in job:
            wanted = set(services.split(','))
            job['MachServices'] = {k: v for k, v in job['MachServices'].items() if k in wanted}
            print(f'  job {label}: only {", ".join(sorted(job["MachServices"]))}')
    job['StandardErrorPath'] = '/dev/console'
    # a job that is named is wanted, whatever hardware it says it is for: wifid's asks for a node "wlan" in the
    # device tree, which the loader's tree does not have yet
    job.pop('_LimitLoadToDeviceTree', None)
    if 'all' in oslog or label in oslog:
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    with open(f'{dest}/{label}.plist', 'wb') as f:
        plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
    program = job.get('Program') or job['ProgramArguments'][0]
    missing = '' if present(program) else '   (NOT in the system image!)'
    print(f'  job {label}: {program}{missing}')

if rest:
    count = 0
    rest_jobs = [(src, name) for name in sorted(os.listdir(src))]
    if os.path.isdir(angels):
        rest_jobs += [(angels, name) for name in sorted(os.listdir(angels))]
    for directory, name in rest_jobs:
        label = name[:-len('.plist')]
        if not name.endswith('.plist') or label in labels or any(word in label for word in DENY):
            continue
        with open(f'{directory}/{name}', 'rb') as f:
            job = plistlib.load(f)
        program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
        if not job.get('MachServices') or not present(program):
            continue
        for key in ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval', 'WatchPaths',
                    'QueueDirectories', 'StartOnMount', 'LaunchEvents'):
            job.pop(key, None)
        job['StandardErrorPath'] = '/dev/console'
        if label in oslog:
            job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        with open(f'{dest}/{name}', 'wb') as f:
            plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
        count += 1
    print(f'  ... and {count} more jobs, started on demand only')
