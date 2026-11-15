#!/usr/bin/env python3
"""The thread samples of a console log (userland/src/inject26.c: boot-arg sample_every=N, or the two at 90
and 150 s), with the frames in one library named from a copy of it that still has its local symbols -- the
library in an image is stripped (llvm-strip -x), the one in its build directory is not, and the offsets are
the same.

  tools/samples.py CONSOLE.log LIBRARY [IMAGE]      IMAGE: the file name in the log (default: LIBRARY's)
  tools/samples.py --profile CONSOLE.log LIBRARY    the functions the samples were in, most first

For libhakios-air, LIBRARY is build/mesa26-linux/build/src/gallium/drivers/freedreno/libhakios-air.dylib
of the build the image was made from."""
import bisect
import collections
import os
import re
import subprocess
import sys

NM = os.environ.get('LLVM_NM', '/usr/lib/llvm-20/bin/llvm-nm')


def symbols(lib):
    out = subprocess.run([NM, '-n', '--defined-only', lib], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1].lower() == 't':
            syms.append((int(p[0], 16), p[2].lstrip('_')))
    return syms


def main(argv):
    profile = argv[:1] == ['--profile']
    if profile:
        argv = argv[1:]
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    log, lib = argv[0], argv[1]
    image = re.escape(argv[2] if len(argv) > 2 else os.path.basename(lib))
    syms = symbols(lib)
    addrs = [a for a, _ in syms]

    def name(off):
        i = bisect.bisect_right(addrs, off) - 1
        return '%s+%#x' % (syms[i][1], off - syms[i][0]) if i >= 0 else hex(off)

    frame = re.compile(r'sample (\S+) t(\d+) (\w+) ' + image + r'\+(0x[0-9a-f]+)')
    count = collections.Counter()
    with open(log, 'rb') as f:
        for line in f.read().decode('utf-8', 'replace').splitlines():
            m = frame.search(line)
            if not m:
                if not profile and re.search(r"sample \S+ t\d+ '", line):
                    print(line.strip())
                continue
            where = name(int(m.group(4), 16))
            if profile:
                if m.group(3) == 'pc':
                    count[where.split('+')[0]] += 1
            else:
                print(m.group(1), 't' + m.group(2), m.group(3), where)
    if profile:
        total = sum(count.values())
        for fn, n in count.most_common(30):
            print('%5d  %5.1f%%  %s' % (n, 100.0 * n / max(total, 1), fn))
        print('%d samples' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
