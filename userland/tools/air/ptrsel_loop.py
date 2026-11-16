#!/usr/bin/env python3
"""ptrsel_loop.py [AIR_RUN]: run tools/air/ptrsel_loop.ll on this machine (softpipe) and check every thread's
output against the same search done here. Exit status 0 when all 16 agree.

The table is in buffer 0 (count 8, then keys 10, 20, 35, 40, 50, 57, 70, 80 with values 101..108); the keys are
not evenly spaced so that a search taking the wrong side of a select, or carrying a wrong offset round its loop,
lands on another value and takes another number of steps."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = [10, 20, 35, 40, 50, 57, 70, 80]
VALS = [101 + i for i in range(8)]


def expect(gid):
    target, length, p, steps, total = gid * 5, len(KEYS), 0, 0, 0
    while True:
        half = length >> 1
        mid = p + half
        after = KEYS[mid] > target
        length = half if after else length - half
        p = p if after else mid
        steps += 1
        total += KEYS[mid]
        if length <= 1:
            break
    return VALS[p] + 1000 * steps + 100000 * (total % 97)


def le32(*words):
    return ''.join(int(w).to_bytes(4, 'little').hex() for w in words)


def main():
    air_run = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        '~/hakios/xnu-12377-hakios-deps/build/mesa-host/build/src/gallium/drivers/freedreno/air_run')
    table = [w for k, v in zip(KEYS, VALS) for w in (k, v)]
    with tempfile.TemporaryDirectory() as tmp:
        bc = os.path.join(tmp, 'ptrsel_loop.bc')
        subprocess.run(['llvm-as', os.path.join(HERE, 'ptrsel_loop.ll'), '-o', bc], check=True)
        out = subprocess.run([air_run, '--block', '16,1,1', '--buffer', '0@0=' + le32(8, 0), '--buffer', '0@8=' + le32(*table[:8]),
                              '--buffer', '0@40=' + le32(*table[8:]), bc], capture_output=True, text=True)
    text = out.stdout + out.stderr
    got = {}
    # air_run prints the written buffer's first 16 words as "  [ N]  DECIMAL  0xHEX", as unsigned numbers
    m = re.search(r'device buffer 1 after the dispatch[^\n]*\n((?:\s*\[\s*\d+\][^\n]*\n){16})', text)
    words = [int(w) for w in re.findall(r'\]\s+(\d+)', m.group(1))] if m else []
    print(text if not words else '', end='')
    bad = 0
    for gid in range(16):
        want = expect(gid)
        have = words[gid] if gid < len(words) else None
        got[gid] = have
        if have != want:
            bad += 1
        print(f'thread {gid:2}: {have} (want {want}){"" if have == want else "  WRONG"}')
    print(f'ptrsel_loop: {16 - bad} of 16 agree')
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
