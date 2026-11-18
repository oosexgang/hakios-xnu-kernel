#!/usr/bin/env python3
"""corecrypto's FIPS self test passing again on the rewritten dyld shared cache.

usage: [PACSTRIP=FILE] fips26.py CACHE_DIR [STORED COMPUTED]   (darwin26/dyld-strip, after pacstrip cache)

At boot launchd runs /usr/libexec/cc_fips_test (boot task "fips", RequireSuccess: a failure stops the
system). Its integrity test computes an HMAC over corecrypto's code and compares it with the value
stored in the library. pacstrip rewrote PAC instructions in that code, so the computed value differs,
and the boot ends with "FIPS_POST failed!". The rewrite is deterministic: the value corecrypto
computes from the rewritten code is printed by the test ("MAC generated: ..."), and put here in place
of the stored one ("In variable: ..."); then the file's page hashes are made again (pacstrip resign).

The values below belong to Darwin 26.6.2 (23G90, iPhone12,1) and to the pacstrip that rewrote the cache.
For another build (Darwin 27.2, 24B119) or another pacstrip, boot once, read the two lines from the log
("In variable" is STORED, "MAC generated" is COMPUTED) and give them as arguments.

PACSTRIP: the pacstrip that resigns, by default arch/arm64/tools/pacstrip (darwin/userland/env.sh builds it).
Run again over a cache that already has the computed value, it makes the page hashes of those files again:
a run that stopped between the two steps is finished, not taken for done.
"""
import os
import subprocess
import sys

STORED = bytes.fromhex('B06786FCC46A8BBCF25DDD5C1CCD33E732292939A0C1CA59BA62F05C634772B2')
COMPUTED = bytes.fromhex('03BB3661D3F751049F94095E02F1DA842595709E6F8610943C2A6A7626548434')
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACSTRIP = os.environ.get('PACSTRIP') or os.path.join(REPO, 'arch/arm64/tools/pacstrip')


def main():
    global STORED, COMPUTED
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    directory = sys.argv[1]
    if len(sys.argv) == 4:
        STORED, COMPUTED = bytes.fromhex(sys.argv[2]), bytes.fromhex(sys.argv[3])
    # (before a byte is written: a value replaced and its pages not hashed again is a cache dyld refuses)
    if not os.access(PACSTRIP, os.X_OK):
        sys.exit(f'fips26: no pacstrip at {PACSTRIP} (darwin/userland/env.sh builds it; or PACSTRIP=FILE)')
    changed = []
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        with open(path, 'r+b') as f:
            data = f.read()
            at = data.find(STORED)
            if at < 0:
                continue
            if data.find(STORED, at + 1) >= 0:
                sys.exit(f'fips26: {name}: the stored value is there more than once')
            f.seek(at)
            f.write(COMPUTED)
            changed.append(path)
            print(f'fips26: {name} at {at:#x}: stored HMAC replaced')
    if not changed:
        # replaced by an earlier run, which may have stopped before its resign: those files are resigned again
        # (it makes the same hashes of the same pages)
        for name in sorted(os.listdir(directory)):
            with open(os.path.join(directory, name), 'rb') as f:
                if COMPUTED in f.read():
                    changed.append(os.path.join(directory, name))
        if not changed:
            sys.exit('fips26: the stored value is not in this cache')
        print(f'fips26: the computed value is there already ({len(changed)} files): their page hashes made again')
    subprocess.run([PACSTRIP, 'resign'] + changed, check=True)


if __name__ == '__main__':
    main()
