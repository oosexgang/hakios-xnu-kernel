#!/usr/bin/env python3
"""Symbolicate kernel addresses: sym.py 0xaddr [0xaddr ...] (uses kernel.syms from nm -n)."""
import sys, bisect, os, subprocess
syms = []
for l in open(os.path.join(os.path.dirname(__file__), 'kernel.syms')):
    p = l.split()
    if len(p) == 3 and p[1] in 'tTsSdDbB':
        syms.append((int(p[0], 16), p[2]))
addrs = [s[0] for s in syms]
for a in sys.argv[1:]:
    v = int(a, 16); i = bisect.bisect_right(addrs, v) - 1
    name = syms[i][1]
    if name.startswith('__Z'):
        name = subprocess.run(['c++filt', name[1:]], capture_output=True, text=True).stdout.strip() or name
    print(f'{a}: {name} + {v - syms[i][0]:#x}')
