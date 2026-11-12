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
        self.write('README.md', 'See [the guide](docs/guide.md#part), `tools/thing.py` and `docs/`.\n'
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

    def test_placeholders_and_patterns_are_not_paths(self):
        self.write('README.md', '`devices/NAME/board.toml`, `soc/SOC.toml`, `kernel/patches/*.patch`, `tools/...`\n')
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
        self.write('base/README.md', '[gone](nowhere.md) `tools/gone.py`\n')
        self.write('README.md', 'x\n')
        code, said, _ = self.run_tool(self.links)
        self.assertEqual(code, 0)
        self.assertIn('2 documents, ok', said)        # (README.md and docs/guide.md: not the one under base/)


COMPONENT = '''[[component]]
id = "thing"
what = "a thing"
source = "https://example.org/thing.git"
pins = ["0123456789abcdef"]
pinned_in = "fetch.sh"
used_for = "tests"
licence = "MIT"
'''


class Components(Sandbox):
    def setUp(self):
        super().setUp()
        self.components = tool('components')
        self.components.ROOT = self.dir
        self.components.MANIFEST = self.dir / 'components.toml'
        self.write('fetch.sh', 'git fetch https://example.org/thing.git 0123456789abcdef\n')

    def problems(self, manifest):
        self.write('components.toml', manifest)
        return '\n'.join(self.components.check(self.components.load()))

    def test_a_pin_the_script_has_passes(self):
        self.assertEqual(self.problems(COMPONENT), '')

    def test_a_pin_the_script_no_longer_has_is_found(self):
        self.write('fetch.sh', 'git fetch https://example.org/thing.git fedcba9876543210\n')
        self.assertIn('thing: fetch.sh does not pin "0123456789abcdef"', self.problems(COMPONENT))

    def test_every_pin_is_looked_for(self):
        self.assertIn('does not pin "a-checksum"',
                      self.problems(COMPONENT.replace('["0123456789abcdef"]', '["0123456789abcdef", "a-checksum"]')))

    def test_what_is_missing_or_twice_is_said(self):
        self.assertIn('thing: "licence" is missing', self.problems(COMPONENT.replace('licence = "MIT"\n', '')))
        self.assertIn('thing: listed twice', self.problems(COMPONENT + COMPONENT))
        self.assertIn("pinned_in 'gone.sh' is not a file", self.problems(COMPONENT.replace('fetch.sh', 'gone.sh')))


class Repository(unittest.TestCase):
    def test_the_readmes_name_the_release_the_kernel_is_made_from(self):
        tag = re.search(r'(?m)^UPSTREAM_TAG=xnu-(\S+)$', (ROOT / 'kernel/UPSTREAM').read_text())[1]
        for name in ('README.md', 'README.zh-CN.md'):
            self.assertIn(f'badge/XNU-{tag}-', (ROOT / name).read_text(), f"{name}'s badge is of another release")

    def test_the_loader_is_its_parts_and_its_readme_names_each(self):
        # every file of loader/src is included by loader.c once, and has its row in the README's table
        unit = (ROOT / 'loader/loader.c').read_text()
        readme = (ROOT / 'loader/README.md').read_text()
        included = re.findall(r'(?m)^#include "src/([\w.]+)"', unit)
        there = sorted(path.name for path in (ROOT / 'loader/src').iterdir())
        self.assertEqual(sorted(included), there)
        self.assertEqual(len(included), len(set(included)))
        for name in included:
            self.assertIn(f'| `src/{name}` |', readme, f'loader/README.md has no row for src/{name}')
            first = (ROOT / 'loader/src' / name).read_text().split('\n')[0]
            self.assertTrue(first.startswith('/* Part of the loader (included by loader.c, not compiled alone): '), name)
        self.assertEqual(readme.count('| `src/'), len(included) + 1)       # (and the row of src/ itself)
        # the drivers' list: every start function it calls is called once
        calls = re.findall(r'(?m)^\t(\w+_(?:init|start))\(\);', (ROOT / 'kernel/overlay/bsd/dev/hakios_drivers.c').read_text())
        self.assertGreater(len(calls), 20)
        self.assertEqual(len(calls), len(set(calls)))

    def test_the_pictures_are_pictures_and_nothing_else(self):
        pictures = sorted((ROOT / 'docs/assets').glob('*.svg'))
        self.assertIn('banner.svg', [path.name for path in pictures])
        for path in pictures:
            text = path.read_text().replace('http://www.w3.org/2000/svg', '')
            for unwanted in ('<script', 'href=', '<image', 'http://', 'https://'):
                self.assertNotIn(unwanted, text, path.name)

    def test_this_repository_passes_both(self):
        for name, arguments in (('check-links', []), ('components', ['check'])):
            done = subprocess.run([str(ROOT / 'tools' / f'{name}.py')] + arguments, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)


if __name__ == '__main__':
    unittest.main()
