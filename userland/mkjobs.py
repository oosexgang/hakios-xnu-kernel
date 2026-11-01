#!/usr/bin/env python3
"""Copy stock Darwin launchd job definitions into the small image, adjusted for the bring-up.

usage: mkjobs.py [--rest] [--root-volume LISTING] DARWIN_ROOTFS DEST_DIR label...

For every label, DARWIN_ROOTFS/System/Library/LaunchDaemons/<label>.plist is copied to DEST_DIR with
its standard error sent to the console, which is where a daemon's last words are read (there
is no log viewer or crash reporter yet). For the jobs named in the environment variable OSLOG
("all" for every one) OS_ACTIVITY_DT_MODE is set too, which makes os_log print there as well:
everything the daemon has to say -- SpringBoard alone says a megabyte in half a minute, which
costs time and pushes everybody else out of the log that comes back from the phone.
The program itself must be there at run time; a warning is printed if it will not be.

What "there at run time" means is the recipe, not what happens to be unpacked on this machine, and the
two recipes differ. With the system image as the root, it is what select-system.py chose, and a daemon it
left out as too big really is absent. With the Darwin volume on the phone's flash as the root (build.sh
FLASHBOOT=1), the root is Apple's whole volume: --root-volume names the technical listing of its image
(sysimage/rootfs.slt) and a program counts as there if Apple ships it. Seven jobs turn on that --
fairplayd, identityservicesd, passd, assistantd, callservicesd, lskdd, searchpartyd -- and were being
refused on a flash boot for a reason true only of the thinned copy here. (fairplayd now runs on the phone
and is never missed from a lookup again, but it does not stay: it exits 0 the moment it starts, says nothing,
and KeepAlive brings it back every ten seconds -- 22 times in a session, not one signal. It wants the IOKit
user client com_apple_driver_FairPlayIOKitUserClient, which this kernel has not got. The empty App Store and
Weather were never its doing: see the network section of README.md.) Eight more, Safari's and
AuthenticationServicesAgent, hang on the App cryptex being unpacked beside it at all (see CRYPTEXES
below); a workspace used to lose those silently, which tools/new-workspace.sh no longer does.

With --rest, the other jobs of Darwin follow (their standard error on the console as well, their os_log
only when named in OSLOG), and with them the "launch angels" (System/Library/LaunchAngels: the
applications without an icon that draw parts of the system's own interface in a process of their
own -- the wallpaper, the keyboard, the now playing controls -- and are reached through a Mach
service like a daemon), but strictly on demand: whatever starts them by
itself (RunAtLoad, KeepAlive) is taken out, so that launchd runs one only when a program asks
for one of its Mach services. That is how an application finds the daemons it needs without
all 490 of them being started at boot. Left out: jobs without Mach services (nothing could
ever start them), jobs whose program will not be there at run time, and those in DENY, which
drive hardware that is not there or do things nobody wants here.
"""
import os
import plistlib
import sys

# Substrings of labels -- and two things to know before adding to this list or believing anything in it.
#
# A daemon here may be misbehaving BECAUSE OF ANOTHER ENTRY, so the tuple cannot be judged one line at a
# time. cloudd was the proof: it looked like a daemon that cost the home screen, on the phone as well as in
# QEMU, until it turned out to be retrying com.apple.nsurlsessiond thousands of times because nsurlsessiond
# was itself denied. With that one word gone, cloudd stops spinning and the home screen comes back. Anything
# written below about a daemon was measured against whatever else was denied at the time, which may no longer
# be what is denied now -- when a note here was written before a word came out, retest before trusting it.
#
# And measure against a control from the same afternoon. Every stalled run of mine happened to be later than
# every good one, and another session's QEMU was running on the same machine, so the obvious reading was that
# the machine was loaded rather than that the daemons did it. It was not: one good run sat squarely between
# two stalled ones. A time-ordered pattern is not a cause; put a control next to the thing you changed.
#
# (nsurlsessiond is not among them either, taken out in the sileo2 copy: it is the
# daemon that carries out background URLSession downloads, and without it an application's download never
# starts and never fails -- Sileo sits on "Queued" for ever. It matters here for a second reason, below:
# cloudd spins on it when it is missing.
# mediaserverd is not among them, for all that there is neither sound nor a camera:
# it is where VideoToolbox decodes for every other process, a HEIC picture included; see src/inject_media.c.
# apsd is not among them either: it was left out from the first day without ever being tried, and it is the
# service the rest of the system misses most -- near a third of every "failed lookup" line in the run logs is
# one of 22 daemons asking for com.apple.apsd, and the whole account stack asks before it will do anything.
# It needs no hardware we have not got, and it serves its clients whether or not it reaches a push courier,
# which it never will: the courier wants a device identity rooted in the Secure Enclave this phone has not
# got (apsd -> mobileactivationd, "Device is probably unactivated"), so the push itself stays out of reach.)
# CommCenter stays, and this is what it cost to find out. Shipping both of its plists is the stock
# arrangement -- they carry the same label and launchd picks by the device tree, so with no "baseband" node
# it takes com.apple.CommCenter-noncellular, the variant Darwin has for a device with no modem -- and it does
# pick it. It then gets a long way: it answers com.apple.commcenter.xpc and .coretelephony.xpc (85 and 18
# failed lookups in a session become none), serves geod its policy, loads the network-extension
# configurations. And then it dies, every time, on one of two IOKit drivers of the cellular path that this
# kernel has not got, and each has its own signal. With CommCenter's own log on, the split is clean over
# 20 crashes: 11 SIGSEGV every one of them right after "Could not find AppleIPAppender service" (matched
# by name; user client ApplePDPHelperInterface -- PDP contexts, traffic-class flows, dedicated bearers),
# and 9 SIGABRT every one of them right after "Unable to communicate with AppleBaseband driver"
# (com.apple.driver.AppleBasebandPCI). That answers the obvious question before anyone spends a week on
# it: stubbing AppleIPAppender alone would remove the SIGSEGV half and leave the SIGABRT half, so it is
# not enough on its own -- and AppleBasebandPCI is an Apple modem on PCIe, which this phone has not got.
# It respawns and does it again: 18 crashes in a seven-minute session where the same image without it has
# none. The boot is not held up -- it reaches the same 118 processes against 119 -- so the trade is 103
# failed lookups a session against a process dying every fifteen seconds for ever, which is a bad one.
# Stubbing the first driver is the kind of work HakiosBattery already is, but the second is an Apple
# baseband on PCIe, and this phone's modem is on the SoC and speaks QMI to a Qualcomm subsystem -- so what
# is behind the driver is missing too, not just the driver.
# cloudd and appstored are IN, and getting there was the most instructive thing in this file.
# They looked like daemons that cost the home screen: with them the boot reached 76-78 processes against
# 119 and not one widget or poster started, and on the phone it was worse still -- 42 processes against
# 128, no home screen, the scenario's applications never opening. The cause was neither of them. cloudd
# was retrying com.apple.nsurlsessiond without ever stopping -- 2351 times in that phone session -- because
# nsurlsessiond was itself in this tuple. With nsurlsessiond shipped, the phone run with both of them in
# reaches 130 processes and 20 widget and poster processes with no crash, against the baseline's 128 and 19:
# better than the image without them. Zero nsurlsessiond retries.
# What each one is actually shown to do, which is less than "it works":
#   appstored -- confirmed. It spawns, and com.apple.appstored.xpc.request, the one service the App Store
#     could not find when it was opened by hand, is answered: 0 failed lookups against 1.
#   cloudd -- the harm is gone, the use is not shown. In that phone session nothing asked for it at all
#     (0 requestor lines against 2352 when it was spinning), so all that is proved is that shipping it no
#     longer costs the boot. In QEMU it does start and behaves: 0 retries, 100 processes, 12 widgets.
# Taking 'cloudd' out of the tuple also lets com.apple.itunescloudd through, which has never been asked
# for in any session measured here.
# The App Store: itunesstored and installcoordinationd are in, and this is exactly how far it gets.
# appstored asks for both the moment the store is opened, and with them it runs clean -- 135 processes,
# 17 widget and poster processes, no crash. The App Store opens and shows its own LOADING spinner and
# stays there. It is NOT the FairPlay wall and NOT a sign-in: the store stack never opens a TCP
# connection at all (trustd, mDNSResponder, geod, nsurlsessiond all do; appstored, itunesstored and
# AppStore do not), so it stops before the network. appstored says why, in one line:
#   [bag] AMSBagActiveAccountProvider: The local account doesn't have a storefront.
#         localAccount = <ACAccount type = iTunesStore | username = local | storefront = (null)>
#   [bag] AMSBagNetworkDataSource: Could not fetch initial cached storefront
#   [Bag] Error creating snapshot of bag: Timed out while waiting for snapshot bag to load
# CAUTION, this paragraph's conclusion was wrong and is left here only so the mistake is not repeated.
# There IS a third route to a storefront and it is the one a SIM-less, never-signed-in iPhone uses: the
# bag server sets it over HTTP. AppleMediaServices holds "X-Set-Apple-Store-Front" and the line
# "Attempting to load the bag again because the response had a x-set-apple-store-front header", plus
# https://bag.itunes.apple.com/bag.xml; AMS fetches the bag with no storefront, the server picks one from
# the request's address, and AMSBagDataSource _setStorefrontFromURLResponse:bagData: writes it onto the
# local account. "The local account doesn't have a storefront" is a fallback message, not a wall, and
# there is no activation gate anywhere in the store stack.
# AND THE MEASUREMENT UNDER IT CANNOT CARRY A NEGATIVE AT ALL, which is the lesson worth keeping:
# /dev/console on the phone DROPS LINES, and every "it never appears in the log" above rests on that
# channel. In the same run the scenario's steps demonstrably ran -- init echoed the command line, the
# step after them printed -- and net_test's own output for resolve, reach and two fetches is simply
# absent, while launchd's "Last log repeated 2312 times" of failed lookups fills the window. So the
# store's missing bag strings are not evidence that no bag was fetched; they are evidence of nothing.
# What the console did carry, and what is therefore true, is the opposite of what was written here:
# THE PHONE'S NETWORK WORKS. en0 is 192.168.9.243, mDNSResponder resolves over TCP to 192.168.9.1:53
# (four queries, answers of 159, 842, 200 and 107 bytes), and trustd, geod, maild, gamed, cloudd,
# StocksWidget and nsurlsessiond all open TLS to Apple and Akamai (17.248.214.x, 17.253.39.x,
# 2.16.56.93, 23.11.40.157, 95.100.176.233). "net_test: en0: flags 0x8802" was read 0.1 s into user
# space, before configd had configured it: com.hakios.wifi does that by itself with wifi=1, so a
# scenario must not ask for DHCP again -- only "waitaddr en0 60".
# The storefront question is therefore OPEN, and the next measurement has to be made over a channel
# that does not drop: ssh to the phone (rc.jobs.ssh) and fetch the bag by hand.
# What was measured afterwards, over a channel that does not drop (net_test, waiting out the start):
# THE BAG IS FETCHED FROM THIS PHONE AND THE SERVER DOES HAND OVER A STOREFRONT.
#   fetch https://bag.itunes.apple.com/bag.xml: status 200, 56219 bytes in 0.53 s
#   fetch https://bag.itunes.apple.com/bag.xml: x-set-apple-store-front: 143469-2,8
# (init.itunes.apple.com answers the same.) So the third route is open: no account, no SIM, no
# activation, and Apple still says which storefront this device is in, picked from the address the
# request came from. What does not happen is the store asking: while the App Store is open and
# spinning, appstored opens NO socket at all -- the kernel's connection log for that session names
# net_test, trustd, akd, cloudd, passd, coreidvd, gamed, geod, maild, promotedcontentd,
# familycircled, StocksWidget and mDNSResponder, and neither appstored nor nsurlsessiond. AMS reads
# an empty cache ("Could not fetch initial cached storefront. error = (null)") in the daemon's first
# 40 ms and then stops; 30 s later the client gets "Timed out while waiting for snapshot bag to
# load". The wall is inside AppleMediaServices, not at Apple and not in the network.
# Where to look next, from AMS's own strings (AMSCore::LoadNetworkBagTask): perform() calls
# getAccount() before anything goes out, and the failure strings around it are "Attempted a bag load
# but an account wasn't provided" and "Failed to fetch local account. This will prevent initial bag
# requests from specifying storefront". Those lines are below the level that OS_ACTIVITY_DT_MODE
# prints, which is why nothing is seen; amsaccountsd is up and serving (it logs appstored's XPC
# connection and its AMSDCookieService.getCookies transactions), so the account layer answers at
# least that much.
# The two services below are still the ones appstored cannot find, but they are no longer the story:
#   com.apple.commcenter.coretelephony.xpc -- the SIM's country, through CommCenter, which crash-loops
#     here (see above);
#   com.apple.mobileactivationd -- the activation record's region. That binary carries RegionCode,
#     RegionInfo and subscriberCountryCodeDidChange:, and it is rooted in a Secure Enclave this phone
#     has not got.
# (Still worth knowing, and unaffected: the setup assistant is not the missing piece, since this port refuses it
# (src/inject_sb.c forces -[SBSetupManager _setSetupRequiredReason:] to 0) -- it is not. We already ship
# com.apple.purplebuddy.plist with SetupDone = True, MobileGestalt already answers RegionInfo = LL/A, and
# a region is not a storefront. AMS has no country-to-storefront path to feed either: every countryCode
# selector in AppleMediaServices derives the country FROM the bag (_countryCodeFromBag:), and the bag
# cannot be fetched without a storefront -- AMSBagNetworkTask takes one as an argument. AMSDefaults holds
# storefrontSuffixes, not a storefront. So there is no file that can be shipped to fake this: it needs
# CommCenter working or an account signed in, and signing in is not a decision to be taken here.
# The one stall that looked like a second cause was the same cause. ALLOW="syncdefaultsd appstored" lost the
# home screen too (77 processes, no widgets) with no retry storm at all, which seemed to rule nsurlsessiond
# out -- but that run was built before nsurlsessiond came out of this tuple. Built again with it there, the
# same pair reaches 112 processes and 15 widget and poster processes, PosterBoard among them. So every stall
# measured in this group traces to the one missing daemon, and nothing here needs a second explanation.
# The thing that nearly hid it: the storm is cloudd's alone. syncdefaultsd and appstored do not spin on
# nsurlsessiond -- they simply wait on something that waits on it -- so "no retry storm" was not evidence of
# a different cause. Absence of the symptom is not absence of the illness.
# syncdefaultsd (com.apple.kvsd) is out on its own merits. It serves cloudkeychainproxy3 its store, then
# aborts inside changeTokenForStoreWithConfiguration:reply: with
#   *** Terminating app due to uncaught exception 'NSInvalidArgumentException',
#       reason: '*** -[NSURL initFileURLWithPath:isDirectory:]: nil string parameter'
# -- it is building a file URL for a directory it was given as nil. The likely one is the SyncedPreferences
# directory it asks for by name ("uncontainerizedSyncedPreferencesDirectoryURL"). Two candidates were
# followed and both are ruled out, so nobody need follow them again.
#   Not a missing directory of mobile's. build.sh can make an empty one with a d line in var.skel, as it
#     does for /var/root/.ssh; with /private/var/mobile/Library/SyncedPreferences created at boot,
#     syncdefaultsd aborts in the same place with the same exception.
#   Not a container lookup either. Running with containermanagerd's own log on, syncdefaultsd never asks
#     it anything: every request in the session comes from lsd. So _container_create_or_lookup_path is
#     not where the nil is born.
# What it looks like instead: the URL is per bundle, and it comes back nil for whichever bundle the
# migration pass happens to reach. syncdefaultsd walks 686 of them ("Checking for plist URLs from bundle:
# ..."), and the one it dies after is a different bundle every run -- com.apple.PaperBoard.LegacyPoster in
# one, com.apple.exchangesyncexpress.framework.EASOAuthService in another -- so it is not one bad bundle.
# It also aborts on the cloudkeychainproxy3 store registration with no bundle in hand at all. Whatever
# supplies a bundle's SyncedPreferences path is empty here, for bundles at large; that is not two lines of
# var.skel, and guessing directories is the wrong tool for it.
# bird and duetexpertd were shipped as well and nothing ever asked for them in a bare boot (no spawn, no
# lookup), so they are still untested rather than known good; they need a session with applications.
# mobileactivationd came out of this tuple the same way apsd did: it had been denied since the first day
# and never tried, and appstored was the one asking (four failed lookups a session, and "[HRNSupport]
# Failed to copy activation record ... MobileActivation.ErrorDomain Code=-1 IPC Error" twenty times from
# every thread that touches a bag). With it in, started on demand: no failed lookups at all, no crash and
# no loop (one spawn a session, 199-200 processes against 198 without it), and the account layer gets an
# answer instead of a broken pipe -- Code=-8 "Device is not activated: Unactivated". That is as far as it
# can go here: the daemon says "Failed to bump UIK: 0xe00002c7" because the activation chain wants a
# Secure Enclave this phone has not got. It does NOT unblock the store: with it in, the bag is still
# never fetched. It is worth keeping for the answer, not for the store.
# (nsurlsessiond is not among them either, for all that it belongs to the same family of Apple's own
# services: it is the daemon that carries out background URLSession downloads,
# and without it an application's download never starts and never fails -- Sileo sits on "Queued"
# for ever, see «Sileo» in README.md.)
# (mediaserverd is not among them, for all that there is neither sound nor a camera:
# it is where VideoToolbox decodes for every other process, a HEIC picture included; see src/inject_media.c.)
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties',
        'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'bird',
        # these start, crash and are asked for again, over and over. (aggregated, which was among them, is where
        # Darwin userland 16 keeps its power log -- com.apple.powerlog.plxpclogger.xpc and the rest -- that Settings > Battery
        # waits for; it now runs through a session.)
        'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'avconferenced',
        # visual voicemail (vmd): with no CommCenter it asks for com.apple.commcenter.coretelephony.xpc hundreds of times a
        # second, a whole processor of the seven and launchd's 2-3% answering it (the Xiaomi 12S, cpu_sample=1, 1 November)
        'voicemail')

# ALLOW="word ..." in the environment lets jobs through that DENY would stop: for trying one out
DENY = tuple(word for word in DENY if word not in os.environ.get('ALLOW', '').split())

args = sys.argv[1:]
rest = '--rest' in args
root_volume = None              # the listing of the whole Darwin volume, when that volume is the root
if '--root-volume' in args:
    i = args.index('--root-volume')
    root_volume = args[i + 1]
    del args[i:i + 2]
args = [a for a in args if a != '--rest']
src_root, dest, labels = args[0], args[1], args[2:]
oslog = os.environ.get('OSLOG', '').split()
src = f'{src_root}/System/Library/LaunchDaemons'
# A program in a cryptex -- on a device a volume of its own, grafted at /System/Cryptexes/NAME (eight of
# Safari's daemons are in the App cryptex) -- is looked for where unpack.sh unpacked that cryptex, beside
# the root filesystem; the images have it at the same place (build.sh).
CRYPTEXES = {'/System/Cryptexes/OS/': 'cryptex', '/System/Cryptexes/App/': 'cryptex-app'}


def volume_paths(listing, _cache={}):
    """Every path in a 7z technical listing of the Darwin root filesystem image, without a leading slash."""
    if listing not in _cache:
        _cache[listing] = {line[len('Path = '):].strip()
                           for line in open(listing, errors='replace') if line.startswith('Path = ')}
    return _cache[listing]


def present(program):
    for prefix, unpacked in CRYPTEXES.items():
        if program.startswith(prefix):
            return os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(src_root)), unpacked,
                                               program[len(prefix):]))
    if root_volume:
        return program.lstrip('/') in volume_paths(root_volume)
    return os.path.isfile(f'{src_root}{program}')


angels = f'{src_root}/System/Library/LaunchAngels'
os.makedirs(dest, exist_ok=True)
for label in labels:
    with open(f'{src}/{label}.plist', 'rb') as f:
        job = plistlib.load(f)
    job['StandardErrorPath'] = '/dev/console'
    # a job that is named is wanted, whatever hardware it says it is for: wifid's asks for a node "wlan" in the
    # device tree, which the loader's tree does not have yet
    job.pop('_LimitLoadToDeviceTree', None)
    if 'all' in oslog or label in oslog:
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    with open(f'{dest}/{label}.plist', 'wb') as f:
        plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
    program = job.get('Program') or job['ProgramArguments'][0]
    missing = '' if present(program) else ('   (NOT on the Darwin volume!)' if root_volume
                                           else '   (NOT in the system image!)')
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
