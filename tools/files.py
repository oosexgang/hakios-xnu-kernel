#!/usr/bin/env python3
"""The repository's files, as the checks go over them: what git tracks in a checkout -- and in a tree with no git, the
one a release's tarball unpacks to, what the release listed (.tracked-files, which scripts/release.sh puts into the
tarball), else what is on disk, without the directories git ignores for what the builds write.

  tools/files.py [-z] [--others] [PATTERN...]

PATTERN as git's pathspec: '*.md' at any depth, a directory with everything under it, a file as it is. --others: the
files git does not track and does not ignore as well (git ls-files -co --exclude-standard); a tree with no git cannot
tell them apart, and gives what it has. -z: each name ended by a NUL instead of a newline.
"""
import fnmatch
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LISTED = '.tracked-files'
# (what is never the repository's in a tree with no git: anywhere, and at its top -- .gitignore's directories of
# build output, logs and device packages brought in)
SKIP_ANYWHERE = {'.git', '__pycache__'}
SKIP_AT_TOP = {'build', 'logs', 'devices'}


def checkout(root):
    """Is ROOT the top of a git checkout (not a tree unpacked inside somebody else's)?"""
    try:
        done = subprocess.run(['git', '-C', str(root), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    except OSError:
        return False
    return done.returncode == 0 and Path(done.stdout.strip()).resolve() == Path(root).resolve()


def matches(name, patterns):
    """git's pathspec, without its magic: the path itself, a directory above it, or a glob whose * crosses /"""
    return not patterns or any(name == p or name.startswith(p.rstrip('/') + '/') or fnmatch.fnmatchcase(name, p)
                               for p in patterns)


def on_disk(root):
    found = []
    for directory, subdirectories, names in os.walk(root):
        top = Path(directory) == Path(root)
        subdirectories[:] = sorted(d for d in subdirectories
                                   if d not in SKIP_ANYWHERE and not (top and d in SKIP_AT_TOP))
        for name in names:
            found.append((Path(directory) / name).relative_to(root).as_posix())
    return sorted(found)


def listed(patterns=(), others=False, root=None, on_disk_too=True):
    """The repository's file names under ROOT, relative and with /, sorted as git sorts them. on_disk_too=False: none
    when there is neither git nor a release's list -- for the question whether a file is the repository's own, which
    what happens to be on disk cannot answer (a package brought into a copy is on disk too)."""
    root = Path(root or ROOT)
    patterns = list(patterns)
    if checkout(root):
        names = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'] + (['-co', '--exclude-standard'] if others else []) +
                               ['--'] + patterns, capture_output=True, check=True).stdout.decode().split('\0')
        return [n for n in names if n]
    if (root / LISTED).is_file():
        names = [n for n in (root / LISTED).read_text().split('\n') if n]
    elif on_disk_too:
        names = on_disk(root)
    else:
        return []
    return [n for n in names if matches(n, patterns)]


def main(argv):
    end = '\n'
    others = False
    patterns = []
    for argument in argv:
        if argument == '-z':
            end = '\0'
        elif argument == '--others':
            others = True
        elif argument.startswith('-'):
            print(f'files.py: unknown option {argument} (tools/files.py [-z] [--others] [PATTERN...])', file=sys.stderr)
            return 2
        else:
            patterns.append(argument)
    sys.stdout.write(''.join(name + end for name in listed(patterns, others)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
