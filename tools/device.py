#!/usr/bin/env python3
"""Device packages: bring a phone's description into this checkout, or take it out again.

  tools/device.py available [--from SOURCE]     the packages a source has
  tools/device.py add NAME... [--from SOURCE]   copy packages into arch/arm64/boot/dts/ and check them
  tools/device.py add --from SOURCE             all the packages SOURCE has: a phone's own repository, its one
  tools/device.py remove NAME...                take them out again
  tools/device.py export NAME DIR               copy the package arch/arm64/boot/dts/NAME to DIR/NAME: into a checkout of
                                                the devices branch, to send it there
  tools/device.py list                          the packages that are here, and where each is from

A device package is a directory NAME with board.dts in it (arch/arm64/boot/dts/_template/board.dts explains the
file) and whatever else belongs to the phone: its README, its notes. The repository itself describes no
phone: packages are kept on its `devices` branch, one directory each, and whoever builds for a phone
brings its package in. What is built -- the kernel, the loader, the libraries -- then knows the boards
that are there (tools/boards.py gen).

SOURCE is where packages are taken from:
  a directory            SOURCE/NAME/board.dts: a checkout of the devices branch, a package of your own
  a repository's URL     URL[#REF] (https://..., ssh://..., git@host:..., file://..., or a path ending in .git):
                         somebody's own repository for their phone, cloned (shallow) into build/device-sources/;
                         REF a branch, tag or commit, its default branch unless told
  a branch or commit     of this repository, with the packages at its top; `devices` unless told
                         otherwise, fetched from origin first when there is an origin to ask
A directory or repository with board.dts at its very top is one package, named as its board.dts names it: a phone's
repository needs nothing else, and may be called anything.

arch/arm64/boot/dts/NAME is not tracked here (.gitignore): a package is changed where it is kept, not in a checkout
of the code. A new phone is made here (tools/boards.py new), and sent with `export`:
  git worktree add ../hakios-devices devices
  tools/device.py export NAME ../hakios-devices
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards  # noqa: E402  (what a board file is, and whether one is right)

BRANCH = 'devices'              # where packages are kept when nothing else is said
NOTE = '.from'                  # arch/arm64/boot/dts/NAME/.from: where a package that was brought in is from
# a line of arch/arm64/boot/dts/_template/README.md that a README somebody has written no longer has
TEMPLATE_LINE = 'One sentence: how far it boots'



def git(*arguments, check=True):
    done = subprocess.run(['git', '-C', str(boards.ROOT)] + list(arguments), capture_output=True, text=True)
    if check and done.returncode != 0:
        raise SystemExit(f'git {" ".join(arguments)}: {done.stderr.strip()}')
    return done


def is_url(given):
    """Is SOURCE another repository's address rather than a directory, branch or commit?"""
    where = given.split('#', 1)[0]
    return '://' in where or re.match(r'^[\w.-]+@[\w.-]+:', where) is not None or where.endswith('.git')


def package_name(directory, text):
    """The name of the one package DIRECTORY is (board.dts at its top): the board's own name, or else the last part
    of TEXT (a repository's address, a directory), without .git, in lower case."""
    try:
        name = boards.boardfile.parse((directory / 'board.dts').read_text(), boards.BOARD_SCHEMA).get('name')
    except (OSError, boards.boardfile.FileError):
        name = None
    if isinstance(name, str) and boards.NAME.match(name):
        return name
    last = re.sub(r'\.git$', '', re.split(r'[/:]', text.rstrip('/'))[-1]).lower()
    return re.sub(r'[^a-z0-9-]+', '-', last).strip('-')


def clone(given):
    """URL[#REF] cloned, shallow, into build/device-sources/: its directory and the commit it is at."""
    url, _, ref = given.partition('#')
    cache = boards.ROOT / 'build/device-sources' / (re.sub(r'[^A-Za-z0-9.-]+', '_', url).strip('_') + (f'@{ref}' if ref else ''))
    if cache.exists():
        shutil.rmtree(cache)            # (taken again whole: what is brought in is what the repository has now)
    cache.parent.mkdir(parents=True, exist_ok=True)
    command = ['git', 'clone', '-q', '--depth', '1'] + (['--branch', ref] if ref else []) + [url, str(cache)]
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0 and ref:
        # a commit, which --branch does not take: the whole history, then that commit
        shutil.rmtree(cache, ignore_errors=True)
        done = subprocess.run(['git', 'clone', '-q', url, str(cache)], capture_output=True, text=True)
        if done.returncode == 0:
            done = subprocess.run(['git', '-C', str(cache), 'checkout', '-q', ref], capture_output=True, text=True)
    if done.returncode != 0:
        shutil.rmtree(cache, ignore_errors=True)
        raise SystemExit(f'{given}: the repository cannot be cloned: {done.stderr.strip()}')
    commit = subprocess.run(['git', '-C', str(cache), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return cache, commit


class Source:
    """Where packages are taken from: a directory, another repository, or a commit of this repository."""

    def __init__(self, given):
        self.directory = self.commit = self.single = None
        # (a repository's address; a bare repository on this machine is one too, a checkout is a directory)
        if given is not None and is_url(given) and (not Path(given).is_dir() or (Path(given) / 'HEAD').is_file()):
            self.directory, commit = clone(given)
            self.name = f'{given} ({commit[:12]})'
            self.single = package_name(self.directory, given.split('#', 1)[0]) if (self.directory / 'board.dts').exists() else None
            return
        if given is not None and Path(given).is_dir():
            self.directory = Path(given).resolve()
            self.name = str(given)
            self.single = package_name(self.directory, str(self.directory)) if (self.directory / 'board.dts').exists() else None
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
        if self.single is not None:
            return [self.single]
        if self.directory is not None:
            return sorted(path.parent.name for path in self.directory.glob('*/board.dts')
                          if not path.parent.name.startswith('_'))
        listed = git('ls-tree', '-r', '--name-only', self.commit).stdout.split('\n')
        return sorted(name.split('/')[0] for name in listed
                      if name.count('/') == 1 and name.endswith('/board.dts') and not name.startswith('_'))

    def copy(self, name, to):
        """The package's files into the directory `to`, which is not there yet."""
        if self.directory is not None:
            # (a phone's repository's own: its git, its checks, its images, which no build reads)
            shutil.copytree(self.directory if self.single is not None else self.directory / name, to,
                            ignore=shutil.ignore_patterns('.git', '.github', 'images', NOTE))
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
    """Is arch/arm64/boot/dts/NAME part of the repository itself (the emulator's board, the templates)?"""
    return bool(git('ls-files', '--', f'arch/arm64/boot/dts/{name}', check=False).stdout.strip())


def described(directory, name, board=None):
    """A package's one line: its name, SoC and description, as its board file (DIRECTORY/NAME/board.dts) has them."""
    try:
        data = boards.boardfile.parse((board or directory / name / 'board.dts').read_text(), boards.BOARD_SCHEMA)
    except (OSError, boards.boardfile.FileError):
        return f'{name:14} (its board.dts cannot be read)'
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
            raise SystemExit(f'arch/arm64/boot/dts/{name} is part of the repository itself: not replaced')
        if target.exists():
            if not (target / NOTE).exists():
                raise SystemExit(f'arch/arm64/boot/dts/{name} is there and was not brought in by this tool: it may be work of '
                                 f'yours. Move it away first (or tools/device.py remove {name})')
            shutil.rmtree(target)
        source.copy(name, target)
        (target / NOTE).write_text(f'{source.name}\n')
        print(f'arch/arm64/boot/dts/{name}: from {source.name}')
    problems = boards.Problems()
    boards.load(problems)
    mine = [problem for problem in problems if any(f'arch/arm64/boot/dts/{name}/' in problem for name in names)]
    for problem in mine:
        print(problem, file=sys.stderr)
    if mine:
        print('the package does not fit this checkout: a package and the code change together when a key is added '
              '(arch/arm64/boot/dts/_template/board.dts has every key of this checkout)', file=sys.stderr)
        return 1
    for name in names:
        print(f'next: tools/boards.py status {name}; the builds make their tables themselves')
    return 0


def remove(names):
    for name in names:
        target = boards.DEVICES / name
        if not boards.NAME.match(name) or not (target / 'board.dts').exists():
            raise SystemExit(f'there is no package arch/arm64/boot/dts/{name}')
        if tracked(name):
            raise SystemExit(f'arch/arm64/boot/dts/{name} is part of the repository itself: not removed')
        shutil.rmtree(target)
        print(f'arch/arm64/boot/dts/{name}: removed')
    return 0


def export(name, directory):
    """The package as it is here to DIR/NAME, checked first: what is sent is what was validated."""
    source = boards.DEVICES / name
    if not boards.NAME.match(name) or not (source / 'board.dts').exists():
        raise SystemExit(f'there is no package arch/arm64/boot/dts/{name}')
    directory = Path(directory)
    if not directory.is_dir():
        raise SystemExit(f'{directory} is not a directory (a checkout of the devices branch: git worktree add DIR {BRANCH})')
    problems = boards.Problems()
    boards.load(problems)
    mine = [problem for problem in problems if f'arch/arm64/boot/dts/{name}/' in problem]
    for problem in mine:
        print(problem, file=sys.stderr)
    if mine:
        print(f'arch/arm64/boot/dts/{name} is not right yet: not exported', file=sys.stderr)
        return 1
    target = directory / name
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target, ignore=shutil.ignore_patterns('.git', NOTE))
    readme = target / 'README.md'
    print(f'{target}: the package {name}' + (
        '; it has no README.md yet: say there what works and how it was verified' if not readme.exists() else
        '; its README.md is still the template: say there what works and how it was verified'
        if TEMPLATE_LINE in readme.read_text() else ''))
    return 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('available', 'add'):
        command = commands.add_parser(name)
        command.add_argument('--from', dest='source', metavar='SOURCE')
        if name == 'add':
            command.add_argument('names', nargs='*', metavar='NAME')
    commands.add_parser('remove').add_argument('names', nargs='+', metavar='NAME')
    command = commands.add_parser('export')
    command.add_argument('name', metavar='NAME')
    command.add_argument('directory', metavar='DIR')
    commands.add_parser('list')
    options = parser.parse_args(arguments)

    if options.command == 'available':
        source = Source(options.source)
        names = source.packages()
        print(f'{source.name}: {len(names)} packages')
        for name in names:
            if source.single is not None:
                print('  ' + described(None, name, source.directory / 'board.dts'))
            elif source.directory is not None:
                print('  ' + described(source.directory, name))
            else:
                text = git('show', f'{source.commit}:{name}/board.dts').stdout
                try:
                    data = boards.boardfile.parse(text, boards.BOARD_SCHEMA)
                    print(f'  {name:14} {data.get("soc", "?"):10} {data.get("description", "")}')
                except boards.boardfile.FileError:
                    print(f'  {name:14} (its board.dts cannot be read)')
        return 0
    if options.command == 'add':
        source = Source(options.source)
        if not options.names and options.source is None:
            raise SystemExit('which phone? tools/device.py add NAME (tools/device.py available lists them)')
        # (no NAME with a source of its own -- somebody's phone repository, a directory -- takes all it has)
        return add(options.names or source.packages(), source)
    if options.command == 'remove':
        return remove(options.names)
    if options.command == 'export':
        return export(options.name, options.directory)
    for path in boards.board_files():
        name = path.parent.name
        note = path.parent / NOTE
        where = ('the repository\'s own' if path.parent.parent == boards.DEVICES and tracked(name) else
                 f'from {note.read_text().strip()}' if note.exists() else f'{boards.shown(path.parent)} (not brought in by this tool)')
        print(f'{described(path.parent.parent, name)}    [{where}]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
