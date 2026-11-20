#!/usr/bin/env python3
"""Device packages: bring a phone's description into this checkout, or take it out again.

  tools/device.py available [--from SOURCE]     the packages a source has
  tools/device.py add NAME... [--from SOURCE]   copy packages into arch/arm64/boot/dts/ and check them
  tools/device.py add --from SOURCE             all the packages SOURCE has: a phone's own repository, its one
  tools/device.py remove NAME...                take them out again
  tools/device.py export NAME DIR               copy the package arch/arm64/boot/dts/NAME to DIR/NAME: into a checkout of
                                                the devices branch, to send it there
  tools/device.py list                          the packages that are here, and where each is from
  tools/device.py update [NAME...]              each package brought in again from where it came (all of them when
                                                none is named): what its repository, branch or directory has now
  tools/device.py init DIR --name NAME --soc SOC [--dts FILE] [--description TEXT]
                                                a phone's own repository, made: its package (tools/boards.py new,
                                                the SoC's file copied from a package here that has it) and what
                                                samples/device-repo/ has (BUILDING.md, .gitignore, a check on push)

A device package is a directory NAME with board.dts in it (arch/arm64/boot/dts/_template/board.dts explains the
file) and whatever else belongs to the phone: its soc.dtsi, its README, its notes. What a phone's repository has around
it -- .git, .github/, .gitignore, BUILDING.md, images/ -- is not brought in, and neither is what such a repository
never holds (samples/device-repo/.gitignore: the vendor's firmware and device tree, Apple's files); `export` sends
neither on. Documentation/porting.md, "Or keep it in a repository of your own", has the layout.

The repository itself describes no phone: packages are kept on its `devices` branch, one directory each, and whoever
builds for a phone brings its package in. What is built -- the kernel, the loader, the libraries -- then knows the boards
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
import fnmatch
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards  # noqa: E402  (what a board file is, and whether one is right)
import files  # noqa: E402  (the repository's own files: git's list, or a release's)

BRANCH = 'devices'              # where packages are kept when nothing else is said
NOTE = '.from'                  # arch/arm64/boot/dts/NAME/.from: where a package that was brought in is from
# a line of arch/arm64/boot/dts/_template/README.md that a README somebody has written no longer has
TEMPLATE_LINE = 'One sentence: how far it boots'
SAMPLE = Path(__file__).resolve().parents[1] / 'samples/device-repo'   # (the repository's own, wherever boards look)
# what a phone's repository has around its package, which is the repository's and not the package's: its git, its
# check, how to build from it, its images
AROUND = ('.git', '.github', '.gitignore', 'BUILDING.md', 'images', NOTE)


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
    # (a word that begins with a dash is an option of git's, not an address -- --upload-pack= runs what follows it -- and
    # each is given after -- besides)
    if url.startswith('-') or ref.startswith('-'):
        raise SystemExit(f'{given}: not a repository\'s address (it begins with a dash)')
    # (the ref's own characters too: one of '../..' would have named the repository itself, or its parent, and both are
    # removed below when a clone fails -- the cache is a plain name inside build/device-sources/, or nothing is done)
    sources = (boards.ROOT / 'build/device-sources').resolve()
    name = re.sub(r'[^A-Za-z0-9.-]+', '_', url).strip('_') + ('@' + re.sub(r'[^A-Za-z0-9.-]+', '_', ref) if ref else '')
    cache = sources / name
    if name in ('', '.', '..') or cache.resolve().parent != sources:
        raise SystemExit(f'{given}: not a repository\'s address this can keep a copy of')
    if cache.exists():
        shutil.rmtree(cache)            # (taken again whole: what is brought in is what the repository has now)
    cache.parent.mkdir(parents=True, exist_ok=True)
    command = ['git', 'clone', '-q', '--depth', '1'] + (['--branch', ref] if ref else []) + ['--', url, str(cache)]
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0 and ref:
        # a commit, which --branch does not take: the whole history, then that commit
        shutil.rmtree(cache, ignore_errors=True)
        done = subprocess.run(['git', 'clone', '-q', '--', url, str(cache)], capture_output=True, text=True)
        if done.returncode == 0:
            done = subprocess.run(['git', '-C', str(cache), 'checkout', '-q', ref], capture_output=True, text=True)
    if done.returncode != 0:
        shutil.rmtree(cache, ignore_errors=True)
        raise SystemExit(f'{given}: the repository cannot be cloned: {done.stderr.strip()}')
    commit = subprocess.run(['git', '-C', str(cache), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return cache, commit


def never_held():
    """What a phone's repository never holds, as samples/device-repo/.gitignore names it (the vendor's firmware and
    device tree, Apple's files, what the tools make): (names at the package's top, names anywhere), fnmatch patterns."""
    top, anywhere = [], []
    for line in (SAMPLE / '.gitignore').read_text().splitlines() if (SAMPLE / '.gitignore').is_file() else []:
        line = line.strip()
        if line and not line.startswith(('#', '!')):
            (top if line.startswith('/') else anywhere).append(line.strip('/'))
    return top, anywhere


def package_only(top):
    """For shutil.copytree of a package at TOP: leaves out what is around a package in a repository (AROUND) and what
    such a repository never holds. A package made in a directory of one's own has the vendor's files beside it --
    firmware/, the Android tree it was started from -- where git ignores them and a plain copy would not: they would
    be in the checkout, and `export` would send them on."""
    at_top, anywhere = never_held()

    def ignore(directory, names):
        first = Path(directory).resolve() == Path(top).resolve()
        return [name for name in names if
                (first and (name in AROUND or any(fnmatch.fnmatch(name, pattern) for pattern in at_top))) or
                any(fnmatch.fnmatch(name, pattern) for pattern in anywhere)]
    return ignore


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
            # (not a phone's repository's own -- its git, its checks, its images, which no build reads -- and not the
            # vendor's files that lie beside a package where it is worked on)
            top = self.directory if self.single is not None else self.directory / name
            shutil.copytree(top, to, ignore=package_only(top))
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
    return bool(files.listed([f'arch/arm64/boot/dts/{name}'], root=boards.ROOT, on_disk_too=False))


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
        if target.exists() and not (target / NOTE).exists():
            raise SystemExit(f'arch/arm64/boot/dts/{name} is there and was not brought in by this tool: it may be work of '
                             f'yours. Move it away first (or tools/device.py remove {name})')
        # build the whole package in a hidden sibling (not a package: boards skips a name that begins with a dot), with its
        # .from, then swap it in with one rename -- an interrupted add leaves this staging copy, never a half real package
        staging = target.with_name('.' + name + '.new')
        if staging.exists():
            shutil.rmtree(staging)
        try:
            source.copy(name, staging)
            (staging / NOTE).write_text(f'{source.name}\n')
            if target.exists():
                shutil.rmtree(target)
            staging.rename(target)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        print(f'arch/arm64/boot/dts/{name}: from {source.name}')
    problems = boards.Problems()
    boards.load(problems)
    mine = [problem for problem in problems if any(f'arch/arm64/boot/dts/{name}/' in problem for name in names)]
    for problem in mine:
        print(problem, file=sys.stderr)
    if mine and any('unknown ' in problem or 'is not one the kernel knows' in problem for problem in mine):
        print('the package does not fit this checkout: a package and the code change together when a key is added '
              '(arch/arm64/boot/dts/_template/board.dts has every key of this checkout)', file=sys.stderr)
        return 1
    if mine:
        # (a package that is being written: what it lacks is its own, and is filled in where it is kept)
        print('the package is here and is not right yet: fill in what is named above where the package is kept, then '
              f'bring it in again (tools/device.py update {" ".join(names)}); tools/boards.py check says the same',
              file=sys.stderr)
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
    shutil.copytree(source, target, ignore=package_only(source))
    readme = target / 'README.md'
    print(f'{target}: the package {name}' + (
        '; it has no README.md yet: say there what works and how it was verified' if not readme.exists() else
        '; its README.md is still the template: say there what works and how it was verified'
        if TEMPLATE_LINE in readme.read_text() else ''))
    return 0


def init(directory, name, soc, dts=None, description=None, with_git=True):
    """A phone's own repository at DIRECTORY: its package at the top, and what samples/device-repo/ has."""
    directory = Path(directory)
    if directory.exists() and any(directory.iterdir()):
        raise SystemExit(f'{directory} is there and not empty: a repository is made in a new directory')
    if (boards.DEVICES / name).exists():
        raise SystemExit(f'arch/arm64/boot/dts/{name} is there already: a package of that name is made elsewhere '
                         f'(tools/device.py remove {name}, or another name)')
    # made where tools/boards.py makes packages, beside the others (a SoC's file is copied from one that has it), then
    # moved: nothing of it is left in this checkout
    boards.new_board(name, soc, Path(dts) if dts else None, None, description, moved=True)
    directory.parent.mkdir(parents=True, exist_ok=True)
    if directory.exists():
        directory.rmdir()
    shutil.move(str(boards.DEVICES / name), str(directory))
    for path in sorted(SAMPLE.rglob('*')):
        if path.is_file():
            target = directory / path.relative_to(SAMPLE)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(path.read_text().replace('NAME', name) if path.name == 'BUILDING.md' else path.read_text())
    if with_git:
        subprocess.run(['git', 'init', '-q', str(directory)], check=True)
    print(f'{directory}: the repository of {name}' + (' (git init: commit it, and push it where others can clone it)'
                                                      if with_git else ''))
    for path in sorted(p for p in directory.rglob('*') if p.is_file() and '.git' not in p.relative_to(directory).parts):
        print(f'  {path.relative_to(directory)}')
    print(f'next: fill in {directory}/board.dts and soc.dtsi (Documentation/porting.md, step 1), then '
          f'tools/device.py add --from {directory} brings it into this checkout, where tools/boards.py check and '
          f'tools/boards.py status {name} read it; after a change there, tools/device.py update {name}')
    return 0


def where_from(name):
    """The SOURCE a package was brought in from, as its note has it ("URL (commit)", "devices (commit)", a directory)"""
    note = boards.DEVICES / name / NOTE
    if not note.is_file():
        return None
    text = note.read_text().strip()
    return re.sub(r' \([0-9a-f]{12}\)$', '', text) or None


def update(names):
    here = [path.parent.name for path in sorted(boards.DEVICES.glob('*/board.dts')) if (path.parent / NOTE).is_file()]
    for name in names:
        if name not in here:
            raise SystemExit(f'arch/arm64/boot/dts/{name} was not brought in by this tool: there is nowhere to take it again from')
    names = names or here
    if not names:
        print('no package here was brought in by this tool (tools/device.py add NAME)')
        return 0
    status = 0
    for name in names:
        source = where_from(name)
        print(f'{name}: from {source}')
        status |= add([name], Source(source))
    return status


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
    commands.add_parser('update').add_argument('names', nargs='*', metavar='NAME')
    command = commands.add_parser('init')
    command.add_argument('directory', metavar='DIR', help='where: a directory that is not there yet, or is empty')
    command.add_argument('--name', required=True, help='the package\'s name, lower-case letters, digits and dashes: the '
                         'phone\'s codename (what board.dts is given as its name)')
    command.add_argument('--soc', required=True, help='the chip\'s name, in lower case (sm7325): soc.dtsi is copied from a '
                         'package here that has it, else started new')
    command.add_argument('--dts', metavar='FILE', help='the phone\'s Android device tree as source, to start from what it '
                         'says (dtc -I dtb -O dts fdt.dtb > phone.dts)')
    command.add_argument('--description', metavar='TEXT', help='"Maker Model", for people')
    command.add_argument('--no-git', action='store_true', help='the directory only, without git init')
    options = parser.parse_args(arguments)
    if options.command == 'update':
        return update(options.names)
    if options.command == 'init':
        return init(options.directory, options.name, options.soc, options.dts, options.description, not options.no_git)

    if options.command == 'available':
        source = Source(options.source)
        names = source.packages()
        print(f'{source.name}: {len(names)} package{"" if len(names) == 1 else "s"}')
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
        # (asked before the devices branch is looked for: with neither, that there is no such branch is not the answer)
        if not options.names and options.source is None:
            raise SystemExit('which phone? tools/device.py add NAME (tools/device.py available lists those of the '
                             f'{BRANCH} branch), or add --from SOURCE: a phone\'s own repository or directory')
        source = Source(options.source)
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
