#!/usr/bin/env python3
"""
mkhighrate.py SYSROOT: CADisableMinimumFrameDurationOnPhone = YES in the Info.plist of every application of the system
image being made (SpringBoard, /Applications, /System/Library/CoreServices).

On a phone CoreAnimation keeps a process's frames at most 60 a second unless its bundle has that key (QuartzCore reads
it; the iPhones with a 120 Hz panel ship their applications with it). Without it the home screen and every application
ran at 60 on this 120 Hz panel even once CoreAnimation knew the panel's rate (fps_info: maximumFramesPerSecond 120;
tools/frames.py: frames every 16.9 ms, 30 October).

SYSROOT is hard links (or clones) of sysimage/rootfs: a plist is written as a new file in place of the link, never
into it, so the extracted system stays as it came out of the IPSW.
"""
import glob
import os
import plistlib
import sys

KEY = 'CADisableMinimumFrameDurationOnPhone'

root = sys.argv[1]
changed = already = 0
for path in sorted(glob.glob(os.path.join(root, 'Applications', '*.app', 'Info.plist')) +
                   glob.glob(os.path.join(root, 'System', 'Library', 'CoreServices', '*.app', 'Info.plist'))):
    with open(path, 'rb') as f:
        data = f.read()
    info = plistlib.loads(data)
    if info.get(KEY) is True:
        already += 1
        continue
    info[KEY] = True
    binary = data.startswith(b'bplist')
    mode = os.stat(path).st_mode & 0o7777
    os.unlink(path)                                     # the link goes; the original stays as it was
    with open(path, 'wb') as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML)
    os.chmod(path, mode)
    changed += 1
print(f'mkhighrate.py: {KEY} set in {changed} applications ({already} had it already)')
