#!/usr/bin/env python3
"""sampler_builder.py [AIR_RUN]: run tools/air/sampler_builder.ll on this machine (softpipe) and check every thread's
output. Exit status 0 when all 16 agree.

The kernel gets its texture, sampler and transform back out of a struct a function call returned (the shape of
CoreImage's stitched kernels, whose `_ci_sampler_builder` does that). Buffer 0 holds a decoy transform at 0 (the
identity), the bias at 16 and the real transform at 32 (mirror x): a transform pointer that lost its offset reads
another texel, as does a texture or a sampler the struct did not carry. Thread (3, 0) returns early through a call
returning an i1 (CoreImage's `_ci_early_out`), so its word is the harness's fill."""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def f4(*v):
    return struct.pack('<4f', *v).hex()


def expect(x, y):
    if (x, y) == (3, 0):                   # returns early: air_run's fill, byte k = (k * 7 + 13) & 0xff
        return int.from_bytes(bytes((k * 7 + 13) & 0xff for k in range(12, 16)), 'little')
    tx, ty = 3 - x, min(y + 1, 3)          # air_run's texel (x, y) is (100 + x, 200 + y, ...)
    return (100 + tx) * 1000 + 200 + ty


def main():
    air_run = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        '~/hakios/xnu-12377-hakios-deps/build/mesa-host/build/src/gallium/drivers/freedreno/air_run')
    # in texels: air_run's sampler for a kernel takes unnormalized coordinates
    args = f4(1, 1, 0.5, 0.5) + f4(0, 1, 0, 0) + f4(-1, 1, 3.5, 0.5)
    with tempfile.TemporaryDirectory() as tmp:
        bc = os.path.join(tmp, 'sampler_builder.bc')
        subprocess.run(['llvm-as', os.path.join(HERE, 'sampler_builder.ll'), '-o', bc], check=True)
        out = subprocess.run([air_run, '--buffer', '0@0=' + args, bc], capture_output=True, text=True)
    text = out.stdout + out.stderr
    m = re.search(r'device buffer 1 after the dispatch[^\n]*\n((?:\s*\[\s*\d+\][^\n]*\n){16})', text)
    words = [int(w) for w in re.findall(r'\]\s+(\d+)', m.group(1))] if m else []
    print(text if not words else '', end='')
    bad = 0
    for i in range(16):
        x, y = i % 4, i // 4
        want, have = expect(x, y), words[i] if i < len(words) else None
        bad += have != want
        print(f'thread ({x}, {y}): {have} (want {want}){"" if have == want else "  WRONG"}')
    print(f'sampler_builder: {16 - bad} of 16 agree')
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
