#!/usr/bin/env python3
"""
Pack xnu-flashed-slot_a.img: mtkloader, then Android's kernel, then the XNU payload, in the
rooted Android boot image (backup/boot_a_magisk.img by default; its ramdisk, dtb and cmdline
are kept). Booting it runs XNU once, then Android comes back with XNU's log in
/sys/fs/pstore/console-ramoops-0 (see mtkloader/mtkloader.c).

  build_flashed.py [android-boot.img]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

android = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'backup', 'boot_a_magisk.img')
subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'chainboot.py'), android,
                       os.path.join(HERE, 'mtkloader.bin'), os.path.join(ROOT, 'xnu-flashed-slot_a.img'),
                       os.path.join(ROOT, 'build-payload.bin')])
