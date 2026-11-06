#!/usr/bin/env python3
"""Show stock iOS launchd jobs: jobinfo.py [-s SERVICE | LABEL...]
With -s, find the jobs that provide a Mach service whose name contains SERVICE.
The jobs are those of System/Library/LaunchDaemons and LaunchAngels."""
import glob, json, os, plistlib, sys
ROOTFS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'ios165/rootfs')
DIRS = [ROOTFS + '/System/Library/LaunchDaemons', ROOTFS + '/System/Library/LaunchAngels']
def show(path):
    d = plistlib.load(open(path, 'rb'))
    prog = d.get('Program') or (d.get('ProgramArguments') or ['?'])[0]
    keys = {k: d[k] for k in d if k in ('UserName', 'RunAtLoad', 'KeepAlive', 'LaunchEvents', 'Sockets', '_LimitLoadToBootMode',
                                         'LimitLoadToHardware', 'LimitLoadFromHardware', 'EnableTransactions', 'ProcessType',
                                         '_AdditionalProperties', 'EnvironmentVariables', 'ProgramArguments')}
    exists = os.path.isfile(ROOTFS + prog)
    print(f"{os.path.basename(path)[:-6]}: {prog}{'' if exists else '  [not extracted]'}")
    print('   ', json.dumps(keys, default=str)[:300])
    print('    services:', ' '.join(sorted(d.get('MachServices', {})))[:400])
if sys.argv[1:2] == ['-s']:
    for path in sorted(path for d in DIRS for path in glob.glob(d + '/*.plist')):
        d = plistlib.load(open(path, 'rb'))
        if any(sys.argv[2] in name for name in d.get('MachServices', {})):
            show(path)
else:
    for label in sys.argv[1:]:
        show(next((f'{d}/{label}.plist' for d in DIRS if os.path.isfile(f'{d}/{label}.plist')), f'{DIRS[0]}/{label}.plist'))
