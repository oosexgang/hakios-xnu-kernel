#!/usr/bin/env python3
"""The repository's own checks, checked: tools/check-links.py finds a link and a path that lead nowhere
and nothing else, tools/components.py finds a pin that the fetching script no longer has."""
import contextlib
import importlib.util
import io
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tool(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Sandbox(unittest.TestCase):
    """A small repository of its own, which the tool is pointed at."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        subprocess.run(['git', 'init', '-q', str(self.dir)], check=True)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def write(self, name, text):
        path = self.dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        subprocess.run(['git', '-C', str(self.dir), 'add', name], check=True)

    def run_tool(self, module, *arguments):
        said, complained = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(complained):
            code = module.main(*arguments)
        return code, said.getvalue(), complained.getvalue()


class Links(Sandbox):
    def setUp(self):
        super().setUp()
        self.links = tool('check-links')
        self.links.ROOT = self.dir
        self.write('docs/guide.md', 'A guide.\n')
        self.write('tools/thing.py', '')

    def test_what_is_there_passes(self):
        self.write('README.md', 'See [the guide](docs/guide.md#part), `tools/thing.py` and `Documentation/`.\n'
                                'Elsewhere: [a site](https://example.org), [above](#top), `some/other/path`.\n')
        code, said, complained = self.run_tool(self.links)
        self.assertEqual((code, complained), (0, ''))
        self.assertIn('2 documents, ok', said)

    def test_a_link_that_leads_nowhere_is_named_with_its_line(self):
        self.write('README.md', 'Fine.\nSee [the guide](docs/gone.md).\n')
        code, _, complained = self.run_tool(self.links)
        self.assertEqual(code, 1)
        self.assertIn('README.md:2: link to docs/gone.md, which is not there', complained)

    def test_links_are_relative_to_the_document(self):
        self.write('docs/other.md', '[up](../README.md) and [beside](guide.md), not [this](docs/guide.md).\n')
        self.write('README.md', 'x\n')
        code, _, complained = self.run_tool(self.links)
        self.assertEqual(code, 1)
        self.assertEqual(complained.count('\n'), 1, complained)
        self.assertIn('docs/other.md:1: link to docs/guide.md', complained)

    def test_a_path_in_backticks_must_exist(self):
        self.write('README.md', 'Run `tools/gone.py`, not `tools/thing.py`.\n')
        code, _, complained = self.run_tool(self.links)
        self.assertEqual(code, 1)
        self.assertIn('README.md:1: `tools/gone.py` is not in the repository', complained)
        self.assertNotIn('thing.py', complained)

    def test_what_the_builds_make_may_be_named_before_it_is_made(self):
        # (what .gitignore keeps out at a place of its own: a fresh checkout, or a release's tree, has none of it yet)
        self.write('.gitignore', '/tools/made.h\n/tools/out/\n/tools/fw-*/\n*.o\n')
        self.write('README.md', '`tools/made.h`, `tools/out/x.bin`, `tools/fw-bt/`; not `tools/other.h`, nor `tools/a.o`\n')
        code, _, complained = self.run_tool(self.links)
        self.assertEqual(code, 1)
        self.assertNotIn('made.h', complained)
        self.assertNotIn('x.bin', complained)
        self.assertNotIn('fw-bt', complained)
        self.assertIn('`tools/other.h` is not in the repository', complained)
        self.assertIn('`tools/a.o` is not in the repository', complained, 'a pattern of no place of its own is no path')

    def test_placeholders_and_patterns_are_not_paths(self):
        self.write('README.md', '`arch/arm64/boot/dts/NAME/board.dts`, `docs/NAME.md`, `kernel/patches/*.patch`, `tools/...`\n')
        self.assertEqual(self.run_tool(self.links)[0], 0)

    def test_paths_of_the_old_layout_may_be_named_between_the_marks(self):
        self.write('README.md', '<!-- paths of the old layout -->\n`tools/moved.py` became `tools/thing.py`; '
                                '[but links](docs/gone.md) are checked.\n<!-- end -->\n`tools/moved.py`\n')
        code, _, complained = self.run_tool(self.links)
        self.assertEqual(code, 1)
        self.assertIn('README.md:2: link to docs/gone.md', complained)
        self.assertNotIn('README.md:2: `tools/moved.py`', complained)
        self.assertIn('README.md:4: `tools/moved.py` is not in the repository', complained)

    def test_the_first_generation_is_left_out(self):
        self.write('userland/README.md', '[gone](nowhere.md) `tools/gone.py`\n')
        self.write('README.md', 'x\n')
        code, said, _ = self.run_tool(self.links)
        self.assertEqual(code, 0)
        self.assertIn('2 documents, ok', said)        # (README.md and docs/guide.md: not the one under userland/)


COMPONENT = '''	thing {
		what = "a thing";
		source = "https://example.org/thing.git";
		pins = "0123456789abcdef";
		pinned-in = "fetch.sh";
		used-for = "the workbench";
		licence = "MIT";
	};
'''


def manifest(*components):
    """A components file of these nodes."""
    return '/ {\n' + ''.join(components) + '};\n'


class Components(Sandbox):
    def setUp(self):
        super().setUp()
        self.components = tool('components')
        self.components.ROOT = self.dir
        self.components.MANIFEST = self.dir / 'components'
        self.write('fetch.sh', 'git fetch https://example.org/thing.git 0123456789abcdef\n')

    def problems(self, text):
        self.write('components', text)
        return '\n'.join(self.components.check(self.components.load()))

    def test_a_pin_the_script_has_passes(self):
        self.assertEqual(self.problems(manifest(COMPONENT)), '')

    def test_a_pin_the_script_no_longer_has_is_found(self):
        self.write('fetch.sh', 'git fetch https://example.org/thing.git fedcba9876543210\n')
        self.assertIn('thing: fetch.sh does not pin "0123456789abcdef"', self.problems(manifest(COMPONENT)))

    def test_every_pin_is_looked_for(self):
        self.assertIn('does not pin "a-checksum"',
                      self.problems(manifest(COMPONENT.replace('"0123456789abcdef";', '"0123456789abcdef", "a-checksum";'))))

    def test_what_is_missing_or_twice_is_said(self):
        self.assertIn('thing: "licence" is missing', self.problems(manifest(COMPONENT.replace('\t\tlicence = "MIT";\n', ''))))
        self.assertIn('thing: listed twice', self.problems(manifest(COMPONENT, COMPONENT)))
        self.assertIn("pinned-in 'gone.sh' is not a file", self.problems(manifest(COMPONENT.replace('fetch.sh', 'gone.sh'))))


class Repository(unittest.TestCase):
    def test_the_readmes_name_the_release_the_kernel_is_made_from(self):
        tag = re.search(r'(?m)^UPSTREAM_TAG=xnu-(\S+)$', (ROOT / 'kernel/UPSTREAM').read_text())[1]
        self.assertIn(f'xnu-{tag}', (ROOT / 'README.md').read_text(), "README.md names another release")

    def test_the_loader_is_its_parts_and_its_readme_names_each(self):
        # every file of arch/arm64/boot/src is included by loader.c once, and has its row in the README's table
        unit = (ROOT / 'arch/arm64/boot/loader.c').read_text()
        readme = (ROOT / 'arch/arm64/boot/README.md').read_text()
        included = re.findall(r'(?m)^#include "src/([\w.]+)"', unit)
        there = sorted(path.name for path in (ROOT / 'arch/arm64/boot/src').iterdir())
        self.assertEqual(sorted(included), there)
        self.assertEqual(len(included), len(set(included)))
        for name in included:
            self.assertIn(f'| `src/{name}` |', readme, f'arch/arm64/boot/README.md has no row for src/{name}')
            first = (ROOT / 'arch/arm64/boot/src' / name).read_text().split('\n')[0]
            self.assertTrue(first.startswith('/* Part of the loader (included by loader.c, not compiled alone): '), name)
        self.assertEqual(len(re.findall(r'(?m)^\| `src/', readme)), len(included) + 1)     # (and the row of src/ itself)
        # the drivers' list: every start function it calls is called once (under its if (CONFIG_...) or not)
        calls = re.findall(r'(?m)^\t+(\w+_(?:init|start))\(\);', (ROOT / 'drivers/board/drivers.c').read_text())
        self.assertGreater(len(calls), 20)
        self.assertEqual(len(calls), len(set(calls)))

    def test_what_the_loader_and_the_kernel_share_is_written_the_same_in_both(self):
        # The loader and the kernel are built apart and talk through pages of memory: the storage's
        # requests, the battery's state. Each side has its own copy of the page's layout and of the
        # constants; a copy that drifts is found here and not on a phone.
        def sources(*patterns):
            return [path for pattern in patterns for path in sorted(ROOT.glob(pattern))]

        def defines(paths):
            found = {}
            for path in paths:
                for name, value in re.findall(r'(?m)^#define[ \t]+(\w+)[ \t]+(.+?)[ \t]*(?:/\*.*)?$', path.read_text()):
                    found.setdefault(name, re.sub(r'\s+', ' ', value))
            return found

        def structures(paths):
            found = {}
            for path in paths:
                for name, body in re.findall(r'(?ms)^struct (\w+) \{\n(.*?)^\}', path.read_text()):
                    fields = []
                    for declaration in re.sub(r'/\*.*?\*/', '', body, flags=re.S).split(';'):
                        words = declaration.split()
                        if not words:
                            continue
                        # "int32_t a, b" is two fields of one type; a field's own "*" or "[n]" stays with its name
                        first = len(words) - 1 - sum(1 for word in words[:-1] if word.endswith(','))
                        kind = ' '.join(words[:first])
                        fields += [(kind, name.rstrip(',')) for name in words[first:]]
                    found.setdefault(name, fields)
            return found

        loader = sources('arch/arm64/boot/src/*.c', 'arch/arm64/boot/src/*.h')
        kernel = sources('drivers/board/*.c', 'drivers/board/*.h', 'include/board/*.h',
                         'arch/arm64/kernel/*.c', 'arch/arm64/kernel/*.h')
        ours, theirs = defines(loader), defines(kernel)
        both = sorted(set(ours) & set(theirs))
        self.assertGreater(len(both), 15)                               # (the two pages' and the firmware calls')
        for name in both:
            self.assertEqual(ours[name], theirs[name], f'{name} is not the same in the loader and in the kernel')
        for name in ('BLOCK_MAGIC', 'BLOCK_VERSION', 'LOADER_BATTERY_MAGIC', 'LOADER_BATTERY_VERSION'):
            self.assertIn(name, both)
        # the block from which the kernel may write into the port's partition is no number in either: both
        # take it from the board's file, and the kernel refuses a loader whose value is not its own
        self.assertEqual(ours['BLOCK_WRITE_FIRST'], '(PLAT->own.write_first)')
        self.assertIn('hakios_board()->storage.write_first', theirs['UFS_WRITE_FIRST'])
        ufs = (ROOT / 'drivers/board/ufs.c').read_text()
        self.assertIn('ufs_page->write_first != UFS_WRITE_FIRST', ufs)
        for path in loader + [ROOT / 'drivers/board/ufs.c']:
            self.assertIsNone(re.search(r'2042368|25374720|30617594|7c3457ef|fd6854ad', path.read_text()),
                              f'{path.name} has a number of one phone\'s partition')
        ours, theirs = structures(loader), structures(kernel)
        for name in ('loader_block', 'loader_block_part', 'loader_battery'):
            self.assertIn(name, ours)
            self.assertIn(name, theirs)
            self.assertGreater(len(ours[name]), 4)
            self.assertEqual(ours[name], theirs[name], f'struct {name} is not the same in the loader and in the kernel')

    def test_the_loader_writes_to_storage_where_its_readme_says_and_nowhere_else(self):
        places = {}
        for path in sorted((ROOT / 'arch/arm64/boot').rglob('*.[ch]')):
            count = len(re.findall(r'->write_blocks\(', path.read_text()))
            if count:
                places[path.relative_to(ROOT / 'arch/arm64/boot').as_posix()] = count
        self.assertEqual(places, {'src/logstore.c': 1, 'src/dsu_write.c': 1, 'src/ufs_service.c': 1})
        section = (ROOT / 'arch/arm64/boot/README.md').read_text().split('## What it writes to the phone\'s storage')[1]
        for place in places:
            self.assertIn(f'`{place}`', section, f'arch/arm64/boot/README.md does not say that {place} writes to the storage')

    def test_no_driver_has_an_address_of_a_chip(self):
        # Where a block of the SoC is comes from the board's data: a driver that names an address is one
        # phone's. (Offsets inside a block, which are the block's own, are not addresses of a chip.)
        named = []
        for path in sorted((ROOT / 'drivers/board').glob('*.[ch]')):
            text = path.read_text()
            named += [f'{path.name}: {line.strip()}' for line in text.split('\n')
                      if re.search(r'#define\s+\w*(PHYS|_PA|_BASE)\s+\(?0x[0-9a-fA-F]{6,}', line) or
                      re.search(r'ml_io_map(_wcomb)?\(\s*0x', line)]
        self.assertEqual(named, [])

    def test_every_barrier_tells_the_compiler_it_touches_memory(self):
        # a volatile asm without a "memory" clobber is not ordered against the loads and stores around it: a
        # barrier or a cache instruction without one may end up on the wrong side of what it was to separate
        barrier = re.compile(r'__asm__\s+(?:__volatile__|volatile)\s*\(\s*"\s*(dsb|dmb|isb|dc\s)[^;]*?\)\s*;', re.S)
        wrong = []
        kernel_dirs = [ROOT / d for d in ('kernel/overlay', 'arch/arm64/kernel', 'crypto', 'fs')]
        for path in sorted([p for d in kernel_dirs for p in d.rglob('*.[ch]')] + [p for d in kernel_dirs for p in d.rglob('*.cpp')] +
                           list((ROOT / 'arch/arm64/boot').rglob('*.[ch]'))):
            text = path.read_text(errors='replace')
            for found in barrier.finditer(text):
                if '"memory"' not in found.group(0):
                    wrong.append(f'{path.relative_to(ROOT)}:{text.count(chr(10), 0, found.start()) + 1}: {found.group(0)[:60]}')
        self.assertEqual(wrong, [], 'barriers without a "memory" clobber')

    def test_the_pictures_are_pictures_and_nothing_else(self):
        pictures = sorted((ROOT / 'Documentation/assets').glob('*.svg'))
        self.assertIn('banner.svg', [path.name for path in pictures])
        for path in pictures:
            text = path.read_text().replace('http://www.w3.org/2000/svg', '')
            for unwanted in ('<script', 'href=', '<image', 'http://', 'https://'):
                self.assertNotIn(unwanted, text, path.name)

    def test_the_loader_writes_the_boot_line_the_kernel_reads(self):
        # boot_args is written by the loader (src/xnu.h) and read by the kernel through boot.h (kernel/patches/0002). The
        # line's length moves bootFlags and memSizeActual: when the two disagree the kernel sizes its memory
        # from whatever lies past what the loader wrote, and panics in its first second.
        loader = re.findall(r'(?m)^#define BOOT_LINE_LENGTH (\d+)$', (ROOT / 'arch/arm64/boot/src/xnu.h').read_text())
        kernel = re.findall(r'(?m)^\+#define BOOT_LINE_LENGTH (\d+)$',
                            (ROOT / 'kernel/patches/0002-platform-arm64.patch').read_text())
        self.assertEqual(len(loader), 1, 'arch/arm64/boot/src/xnu.h defines BOOT_LINE_LENGTH once')
        self.assertEqual(len(kernel), 1, 'the patch sets boot.h BOOT_LINE_LENGTH once')
        self.assertEqual(loader, kernel, 'the loader and the kernel disagree on the length of the boot line')

    def test_every_defconfig_reads_and_names_arguments_that_are_read(self):
        # arch/arm64/configs: each defconfig gives a line of arguments, its includes found, and each argument is one
        # the kernel reads (Documentation/boot-args.md) or Darwin's side of ours does (its sources say "NAME=")
        defconfig = tool('defconfig')
        documented = set(re.findall(r'(?m)^\| `([^`]+)` \|', (ROOT / 'Documentation/boot-args.md').read_text()))
        darwin_side = '\n'.join(p.read_text(errors='replace') for d in ('components', 'userland/userland/src', 'darwin')
                                for p in (ROOT / d).rglob('*') if p.suffix in ('.c', '.m', '.h') and p.is_file())
        configs = sorted((ROOT / 'arch/arm64/configs').glob('*_defconfig'))
        self.assertGreater(len(configs), 3)
        for path in configs:
            arguments = defconfig.arguments(path.name[:-len('_defconfig')])
            self.assertTrue(arguments, path.name)
            for argument in arguments.values():
                name = argument.split('=', 1)[0]
                if name in ('-v', 'cpus'):      # (XNU's own: the console on the screen, how many cores it starts)
                    continue
                self.assertTrue(name in documented or f'{name}=' in darwin_side,
                                f'{path.name}: {name} is read by nothing here')

    def test_this_repository_passes_both(self):
        for name, arguments in (('check-links', []), ('components', ['check'])):
            done = subprocess.run([str(ROOT / 'tools' / f'{name}.py')] + arguments, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)


if __name__ == '__main__':
    unittest.main()
