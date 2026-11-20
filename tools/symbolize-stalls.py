#!/usr/bin/env python3
"""Symbolize the "tl: t" stacks of long SpringBoard turns with a dyld shared cache.

usage: symbolize-stalls.py DSC RUN.log [RUN.log.live*] [--min-ms 80]
DSC is the main cache file; its .symbols file must be beside it (ipsw extract --dyld).
A frame "Image+0xOFF" is OFF past the image's __TEXT start, so its unslid address is
the __TEXT vmaddr (ipsw dyld macho -l) plus OFF.
"""
import argparse
import functools
import re
import subprocess
import sys


@functools.lru_cache(maxsize=None)
def text_base(dsc, image):
    out = subprocess.run(['ipsw', 'dyld', 'macho', dsc, image, '-l'], capture_output=True, text=True).stdout
    m = re.search(r'addr=0x([0-9a-f]+)-0x[0-9a-f]+ \S+\s+__TEXT\s', out)
    return int(m.group(1), 16) if m else None


@functools.lru_cache(maxsize=None)
def symbol(dsc, address):
    out = subprocess.run(['ipsw', 'dyld', 'a2s', dsc, hex(address), '-d', '--no-color'],
                         capture_output=True, text=True).stdout.strip().splitlines()
    return out[-1].strip() if out else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dsc')
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-ms', type=int, default=80)
    args = ap.parse_args()
    seen = {}
    for path in args.logs:
        with open(path, errors='replace') as f:
            for line in f:
                m = re.search(r'tl: t (\d+) at (\d+) woke (\d+) (.*)', line)
                if m and int(m[1]) >= args.min_ms:
                    seen[(int(m[2]))] = (int(m[1]), int(m[3]), m[4].split())
    for at, (ms, woke, frames) in sorted(seen.items()):
        print(f'{woke / 1e6:.3f}s turn, {ms} ms in, sampled at {at / 1e6:.3f}s')
        for frame in frames[:12]:
            m = re.fullmatch(r'([\w.]+)\+(0x[0-9a-f]+)', frame)
            if m and text_base(args.dsc, m[1]):
                print(f'    {frame:40} {symbol(args.dsc, text_base(args.dsc, m[1]) + int(m[2], 16))}')
            else:
                print(f'    {frame}')


if __name__ == '__main__':
    sys.exit(main())
