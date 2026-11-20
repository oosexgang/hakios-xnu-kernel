#!/usr/bin/env python3
"""Documentation/boot-args.md, written from the kernel's sources: every boot argument the kernel reads, where it is
read, and what the comment beside it says.

  tools/bootargs.py              print the reference
  tools/bootargs.py --write      write Documentation/boot-args.md
  tools/bootargs.py --check      fail if Documentation/boot-args.md is not what the sources say (`make check`)

A boot argument is found by its read (PE_parse_boot_argn, PE_parse_boot_arg_str, TUNABLE) in
kernel/overlay and in the lines kernel/patches add. Its description is the comment in the same file that
names it as `name=`: so an argument is documented by writing that comment, once, where the code is.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'kernel/overlay'


def overlay_files():
    """(file in the repository, its path in the kernel's tree) for every file the kernel's overlay adds: kernel/overlay,
    and the directories of the top kernel/lib.sh's OVERLAY_PLACES maps into the tree (arch/arm64/kernel, crypto, fs/)"""
    places = [(OVERLAY, '')]
    for repo, tree in re.findall(r'^\s*"([^":]+):([^"]+)"\s*$', (ROOT / 'kernel/lib.sh').read_text(), re.M):
        places.append((ROOT / repo, tree + '/'))
    for base, prefix in places:
        for path in sorted(base.rglob('*')):
            if path.is_file() and not (prefix and path.name in ('README.md', 'Kconfig')):
                yield path, prefix + str(path.relative_to(base))
PATCHES = ROOT / 'kernel/patches'
OUT = ROOT / 'Documentation/boot-args.md'

READ = re.compile(r'(?:PE_parse_boot_argn|PE_parse_boot_arg_str|PE_parse_boot_arg)\(\s*"([A-Za-z0-9_]+)"|'
                  r'TUNABLE(?:_WRITEABLE|_DEV_WRITEABLE|_DT)?\([^;]*?"([A-Za-z0-9_]+)"')
COMMENT = re.compile(r'/\*.*?\*/', re.S)
SOURCES = ('.c', '.cpp', '.h', '.s', '.m')


def comment_lines(text):
    """The lines of every block comment, without the comment's own marks."""
    lines = []
    for comment in COMMENT.findall(text):
        for line in comment[2:-2].split('\n'):
            lines.append(re.sub(r'^\s*\*? ?', '', line.rstrip()) if line.strip() not in ('', '*') else '')
        lines.append('')
    return lines


def sentence(name, lines, index, rows_end=False):
    """The sentence of comment line `index` (with the lines that carry on its entry) that names `name`;
    `rows_end`: a row of a table of levels ("11  talks APR") is another entry, not more of this one."""
    line = lines[index]
    indent = len(line) - len(line.lstrip())
    text = line.strip()
    for more in lines[index + 1:index + 6]:
        if more.strip() == '' or re.match(r'\s*[A-Za-z0-9_]+=\S', more) or rows_end and re.match(r'\s*\d+\s{2,}\S', more):
            break
        # a table of arguments: its continuation lines are indented further than the entry itself
        if re.match(r'^\s*' + re.escape(name) + r'=\S*\s{2,}', line) and len(more) - len(more.lstrip()) <= indent:
            break
        text += ' ' + more.strip()
    text = re.sub(r'\s+', ' ', text)
    # the sentence that names the argument, and no more
    start = text.find(name)
    before = max(text.rfind('. ', 0, start), text.rfind('): ', 0, start))
    text = text[before + 2 if before >= 0 else 0:].lstrip(': ')
    end = re.search(r'[.;](?= [A-Z(]|$)', text[text.find(name):])
    if end:
        text = text[:text.find(name) + end.end()]
    return re.sub(r'^\(?[Bb]oot-args? ', '', text).strip()


def describe(name, lines):
    """What a comment says of `name=`: from the line that names it to the end of that entry -- the next
    blank line, the next line that starts another argument, or the end of the sentence. With no `name=`
    anywhere, the first sentence that names the argument at all ("held for sound_secs (default 3)")."""
    named = re.compile(r'(?<![\w-])' + re.escape(name) + r'(?:=|\b(?= *(?:\[|:|and\b|,)))')
    best = ''
    for index, line in enumerate(lines):
        match = named.search(line)
        if match is None or '=' not in line[match.start():match.start() + len(name) + 1] and 'oot-arg' not in line:
            continue
        text = sentence(name, lines, index)
        if len(text) > len(best) and len(text) > len(name) + 4:
            best = text
            if 'oot-arg' in line:
                break                   # (a comment written to say what the argument is wins)
    if not best:
        mentioned = re.compile(r'(?<![\w-])' + re.escape(name) + r'(?![\w-])')
        for index, line in enumerate(lines):
            if mentioned.search(line):
                text = sentence(name, lines, index, rows_end=True)
                # (from the clause that names it: not "looked at; held for ...", nor "and the main ...")
                cut = text.rfind('; ', 0, text.find(name))
                text = re.sub(r'^(?:\.\.\. |and |or |then )+', '', text[cut + 2 if cut >= 0 else 0:])
                if len(text) > len(name) + 4:
                    best = text if text.startswith(name) else text[0].upper() + text[1:]
                    break
    return best[:300].rstrip() + ('…' if len(best) > 300 else '')


def collect():
    """{file: {name: description}} over the overlay and the patches' added lines."""
    found = {}
    for path, tree_path in sorted(overlay_files(), key=lambda pair: pair[1]):
        if path.suffix not in SOURCES:
            continue
        text = path.read_text(errors='replace')
        names = [a or b for a, b in READ.findall(text)]
        if names:
            lines = comment_lines(text)
            found[tree_path] = {name: describe(name, lines) for name in sorted(set(names))}
    for patch in sorted(PATCHES.glob('*.patch')):
        file, added = None, {}
        for line in patch.read_text(errors='replace').split('\n'):
            if line.startswith('+++ b/'):
                file = line[6:]
            elif line.startswith('+') and not line.startswith('+++') and file:
                added.setdefault(file, []).append(line[1:])
        for file, lines in added.items():
            text = '\n'.join(lines)
            names = [a or b for a, b in READ.findall(text)]
            if names:
                comments = comment_lines(text)
                found.setdefault(file, {}).update({name: describe(name, comments) for name in sorted(set(names))})
    return found


B = 'bsd/dev/board/'
AREAS = [
    ('Display, graphics and input', ('iokit/Kernel/MiniIOMobileFramebuffer', 'iokit/Kernel/MiniIOSurface', 'iokit/Kernel/BoardGPU',
                                     B + 'adreno', B + 'backlight', B + 'input', B + 'drivers', B + 'goodix', B + 'fts', B + 'ft5x06',
                                     B + 'buttons', B + 'virtio_input', B + 'imu')),
    ('Power, CPU and temperature', (B + 'cpufreq', B + 'scmi_perf', B + 'ddr', B + 'bench', B + 'thermal', B + 'cputop',
                                    B + 'battery', B + 'rtc', 'iokit/Kernel/BoardBattery', 'osfmk/arm64/cpu_idle', 'osfmk/arm64/psci_smp',
                                    'iokit/Kernel/GenericARMPE', 'iokit/Kernel/IOPMrootDomain')),
    ('Sound, camera, haptics, flashlight', (B + 'adsp', B + 'audio', B + 'sound', B + 'speaker', B + 'guard', B + 'mic',
                                            B + 'virtio_sound', B + 'camera', B + 'haptic', B + 'torch')),
    ('Wi-Fi, Bluetooth and network', (B + 'wifi', B + 'wlfw', B + 'qmi', B + 'lx', B + 'sta', B + 'net', B + 'virtio_net',
                                      B + 'apple80211', B + 'bt_uart', 'iokit/Kernel/BoardWLAN', 'iokit/Kernel/MiniIONetworkStack')),
    ('Storage and file systems', (B + 'ufs', B + 'virtio_blk', B + 'sparse', B + 'image', B + 'apfs_check', 'bsd/miscfs/',
                                  'bsd/dev/memdev', 'bsd/vfs/')),
]
REST = 'Kernel core, processes and diagnostics'


def area_of(file):
    for title, prefixes in AREAS:
        if any(file.startswith(prefix) for prefix in prefixes):
            return title
    return REST


def cell(text):
    return text.replace('|', '\\|') if text else '—'


def render(found):
    total = sum(len(names) for names in found.values())
    bare = sum(1 for names in found.values() for text in names.values() if not text)
    out = ['# Boot arguments',
           '',
           '<!-- GENERATED by tools/bootargs.py from kernel/overlay and kernel/patches: do not edit. `make docs` rewrites it. -->',
           '',
           f'Every boot argument the kernel reads: {total} of them, by the file that reads them. The text is the comment',
           'beside the code, so the file named in each heading has the full explanation.',
           '',
           '- Pass them with `--boot-arg NAME=VALUE` to the boot tool, or in the loader\'s command line.',
           '- An argument that is not given leaves its feature off unless the text says otherwise: drivers of a phone\'s',
           '  hardware do nothing until asked.',
           *([f'- {bare} arguments have no comment naming them yet and show "—".'] if bare else []),
           '- To document an argument, write a comment with `name=` in it next to where it is read, then run `make docs`.',
           '- Apple\'s own boot arguments (`debug=`, `serial=`, `-v` ...) work as in any XNU and are not listed.',
           '']
    by_area = {}
    for file, names in found.items():
        by_area.setdefault(area_of(file), []).append((file, names))
    for title in [t for t, _ in AREAS] + [REST]:
        if title not in by_area:
            continue
        out += [f'## {title}', '']
        for file, names in sorted(by_area[title]):
            out += [f'### `{file}`', '', '| Argument | What the source says |', '| --- | --- |']
            out += [f'| `{name}` | {cell(text)} |' for name, text in sorted(names.items())]
            out.append('')
    return '\n'.join(out)


def main():
    text = render(collect())
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print(f'{OUT.relative_to(ROOT)} is not what the sources say: run `make docs`', file=sys.stderr)
            return 1
        print(f'{OUT.relative_to(ROOT)}: up to date')
    elif '--write' in sys.argv:
        OUT.parent.mkdir(exist_ok=True)
        OUT.write_text(text)
        print(f'wrote {OUT.relative_to(ROOT)}')
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
