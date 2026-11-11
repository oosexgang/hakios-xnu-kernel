#!/usr/bin/env python3
"""Device packages: bring a phone's description into this checkout, or take it out again.

  tools/device.py available [--from SOURCE]     the packages a source has
  tools/device.py add NAME... [--from SOURCE]   copy packages into devices/ and check them
  tools/device.py remove NAME...                take them out again
  tools/device.py list                          the packages that are here, and where each is from

A device package is a directory NAME with board.toml in it (devices/_template/board.toml explains the
file) and whatever else belongs to the phone: its README, its notes. The repository itself describes no
phone: packages are kept on its `devices` branch, one directory each, and whoever builds for a phone
brings its package in. What is built -- the kernel, the loader, the libraries -- then knows the boards
that are there (tools/boards.py gen).

SOURCE is where packages are taken from:
  a directory            SOURCE/NAME/board.toml: a checkout of the devices branch, a package of your own
  a branch or commit     of this repository, with the packages at its top; `devices` unless told
                         otherwise, fetched from origin first when there is an origin to ask

devices/NAME is not tracked here (.gitignore): a package is changed where it is kept, not in a checkout
of the code. To send a new phone, copy its directory to the devices branch.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards  # noqa: E402  (what a board file is, and whether one is right)

BRANCH = 'devices'              # where packages are kept when nothing else is said
NOTE = '.from'                  # devices/NAME/.from: where a package that was brought in is from


def git(*arguments, check=True):
    done = subprocess.run(['git', '-C', str(boards.ROOT)] + list(arguments), capture_output=True, text=True)
    if check and done.returncode != 0:
        raise SystemExit(f'git {" ".join(arguments)}: {done.stderr.strip()}')
    return done


class Source:
    """Where packages are taken from: a directory, or a commit of this repository."""

    def __init__(self, given):
        self.directory = self.commit = None
        if given is not None and Path(given).is_dir():
            self.directory = Path(given).resolve()
            self.name = str(given)
            return
        ref = given or BRANCH
        if given is None or '/' not in ref:
            # the branch as its origin has it now, where there is an origin that has it
            if git('fetch', '-q', 'origin', ref, check=False).returncode == 0:
                ref = 'FETCH_HEAD'
        found = git('rev-parse', '-q', '--verify', f'{ref}^{{commit}}', check=False)
        if found.returncode != 0:
            raise SystemExit(f'{given or BRANCH} is neither a directory nor a branch or commit of this repository '
                             f'(packages are kept on the branch "{BRANCH}": git fetch origin {BRANCH})')
        self.commit = found.stdout.strip()
        self.name = f'{given or BRANCH} ({self.commit[:12]})'

    def packages(self):
        if self.directory is not None:
            return sorted(path.parent.name for path in self.directory.glob('*/board.toml')
                          if not path.parent.name.startswith('_'))
        listed = git('ls-tree', '-r', '--name-only', self.commit).stdout.split('\n')
        return sorted(name.split('/')[0] for name in listed
                      if name.count('/') == 1 and name.endswith('/board.toml') and not name.startswith('_'))

    def copy(self, name, to):
        """The package's files into the directory `to`, which is not there yet."""
        if self.directory is not None:
            shutil.copytree(self.directory / name, to, ignore=shutil.ignore_patterns('.git', NOTE))
            return
        to.mkdir(parents=True)
        for path in git('ls-tree', '-r', '--name-only', self.commit, '--', f'{name}/').stdout.split('\n'):
            if not path:
                continue
            content = subprocess.run(['git', '-C', str(boards.ROOT), 'show', f'{self.commit}:{path}'],
                                     capture_output=True, check=True).stdout
            target = to / Path(path).relative_to(name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)


def tracked(name):
    """Is devices/NAME part of the repository itself (the emulator's board, the templates)?"""
    return bool(git('ls-files', '--', f'devices/{name}', check=False).stdout.strip())


def described(directory, name):
    """A package's one line: its name, SoC and description, as its board file has them."""
    try:
        data = boards.tomllib.loads((directory / name / 'board.toml').read_text())
    except (OSError, boards.tomllib.TOMLDecodeError):
        return f'{name:14} (its board.toml cannot be read)'
    return f'{name:14} {data.get("soc", "?"):10} {data.get("description", "")}'


def add(names, source):
    there = source.packages()
    for name in names:
        if not boards.NAME.match(name):
            raise SystemExit(f'"{name}": a package is named in lower-case letters, digits and dashes')
        if name not in there:
            raise SystemExit(f'{source.name} has no package "{name}" (it has: {", ".join(there) or "none"})')
        target = boards.DEVICES / name
        if tracked(name):
            raise SystemExit(f'devices/{name} is part of the repository itself: not replaced')
        if target.exists():
            if not (target / NOTE).exists():
                raise SystemExit(f'devices/{name} is there and was not brought in by this tool: it may be work of '
                                 f'yours. Move it away first (or tools/device.py remove {name})')
            shutil.rmtree(target)
        source.copy(name, target)
        (target / NOTE).write_text(f'{source.name}\n')
        print(f'devices/{name}: from {source.name}')
    problems = boards.Problems()
    boards.load(problems)
    mine = [problem for problem in problems if any(f'devices/{name}/' in problem for name in names)]
    for problem in mine:
        print(problem, file=sys.stderr)
    if mine:
        print('the package does not fit this checkout: a package and the code change together when a key is added '
              '(devices/_template/board.toml has every key of this checkout)', file=sys.stderr)
        return 1
    for name in names:
        print(f'next: tools/boards.py status {name}; the builds make their tables themselves')
    return 0


def remove(names):
    for name in names:
        target = boards.DEVICES / name
        if not boards.NAME.match(name) or not (target / 'board.toml').exists():
            raise SystemExit(f'there is no package devices/{name}')
        if tracked(name):
            raise SystemExit(f'devices/{name} is part of the repository itself: not removed')
        shutil.rmtree(target)
        print(f'devices/{name}: removed')
    return 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('available', 'add'):
        command = commands.add_parser(name)
        command.add_argument('--from', dest='source', metavar='SOURCE')
        if name == 'add':
            command.add_argument('names', nargs='+', metavar='NAME')
    commands.add_parser('remove').add_argument('names', nargs='+', metavar='NAME')
    commands.add_parser('list')
    options = parser.parse_args(arguments)

    if options.command == 'available':
        source = Source(options.source)
        names = source.packages()
        print(f'{source.name}: {len(names)} packages')
        for name in names:
            if source.directory is not None:
                print('  ' + described(source.directory, name))
            else:
                text = git('show', f'{source.commit}:{name}/board.toml').stdout
                try:
                    data = boards.tomllib.loads(text)
                    print(f'  {name:14} {data.get("soc", "?"):10} {data.get("description", "")}')
                except boards.tomllib.TOMLDecodeError:
                    print(f'  {name:14} (its board.toml cannot be read)')
        return 0
    if options.command == 'add':
        return add(options.names, Source(options.source))
    if options.command == 'remove':
        return remove(options.names)
    for path in boards.board_files():
        name = path.parent.name
        note = path.parent / NOTE
        where = ('the repository\'s own' if path.parent.parent == boards.DEVICES and tracked(name) else
                 f'from {note.read_text().strip()}' if note.exists() else f'{boards.shown(path.parent)} (not brought in by this tool)')
        print(f'{described(path.parent.parent, name)}    [{where}]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
