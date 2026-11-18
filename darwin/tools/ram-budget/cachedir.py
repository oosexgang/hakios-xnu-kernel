#!/usr/bin/env python3
"""Where the ram-budget scripts keep what one of them writes for the next: RAM_BUDGET_CACHE, or ram-budget in
XDG_CACHE_HOME (~/.cache), made 0700. The files are pickles, which run code as they are read, so the directory
is one nobody else can write to: a shared /tmp would let another user put one there first.

usage: cachedir.py          (makes it and prints it: RB=$(python3 .../cachedir.py))
"""
import os
import stat
import sys


def path(name=''):
    base = os.environ.get('RAM_BUDGET_CACHE') or os.path.join(
        os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache'), 'ram-budget')
    os.makedirs(base, mode=0o700, exist_ok=True)
    st = os.lstat(base)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid():
        sys.exit(f'ram-budget: {base} is not a directory of yours')
    if st.st_mode & 0o077:
        os.chmod(base, 0o700)
    return os.path.join(base, name) if name else base


if __name__ == '__main__':
    print(path())
