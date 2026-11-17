#!/usr/bin/env python3
"""The kernel's configuration: which of this repository's drivers the kernel starts, as Linux keeps it -- Kconfig files
that describe each choice, fragments that make one, a .config of the result, and a header the code is built with.

  tools/kconfig.py check                         every Kconfig file read, every symbol described once, every choice's
                                                 dependencies and selections known, every CONFIG_ the drivers use
                                                 described; non-zero and the reasons if not
  tools/kconfig.py list [FRAGMENT...]            each symbol, its value in the configuration, and its prompt
  tools/kconfig.py config [FRAGMENT...] [--out FILE]
                                                 the configuration, every symbol in it, as a .config (build/kconfig/.config)
  tools/kconfig.py header [FRAGMENT...] [--out FILE]
                                                 kconfig.h (build/boards/kconfig.h): #define CONFIG_NAME 1 or 0 for every
                                                 symbol, each under #ifndef, so that a test can set one first
  tools/kconfig.py savedefconfig [FRAGMENT...]   the configuration as a fragment: only what differs from the defaults

A FRAGMENT is a file of CONFIG_NAME=y and "# CONFIG_NAME is not set" lines (arch/arm64/configs/kconfig/*.config), or
the name of one there; several are read in order, the later winning. With none, KCONFIG_FRAGMENTS (build.config.arm64)
names them; with that empty, every symbol has its default.

The language is the part of Linux's (Documentation/kbuild/kconfig-language.rst) this tree uses: mainmenu, menu ...
endmenu, if ... endif, source, comment, config and menuconfig with bool, prompt, def_bool, default [if EXPR],
depends on, select and help. A value is y or n: the kernel has no modules. An expression is a symbol, y, n, !EXPR, EXPR && EXPR,
EXPR || EXPR and parentheses. What a fragment asks is taken where the dependencies allow it; a symbol that is selected
is y whatever its own dependencies say (as in Linux, said as a warning).
"""
import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRAGMENTS = ROOT / 'arch/arm64/configs/kconfig'
TOP_KCONFIG = 'Kconfig'


class KconfigError(Exception):
    pass


class Symbol:
    def __init__(self, name, where):
        self.name = name
        self.where = where              # (file, line) of its config entry
        self.prompt = None
        self.visible = []               # the prompt's own `if': where it does not hold, a fragment's ask is not taken
        self.defaults = []              # (value expression, condition expression or None)
        self.depends = []               # expressions, all of which must hold (its own and those of the menus it is in)
        self.selects = []               # (symbol name, condition or None)
        self.help = ''
        self.menu = ''                  # the menu it is in, for the listing


# ---- expressions -------------------------------------------------------------------------------------------------

TOKEN = re.compile(r'\s*(\|\||&&|!=|=|!|\(|\)|[A-Za-z0-9_]+|"[^"]*")')


def parse_expression(text, where):
    tokens = []
    at = 0
    text = text.strip()
    while at < len(text):
        match = TOKEN.match(text, at)
        if not match:
            raise KconfigError(f'{where}: cannot read the expression "{text}"')
        tokens.append(match.group(1))
        at = match.end()
        while at < len(text) and text[at].isspace():
            at += 1
    position = [0]

    def peek():
        return tokens[position[0]] if position[0] < len(tokens) else None

    def take(expected=None):
        token = peek()
        if token is None or (expected is not None and token != expected):
            raise KconfigError(f'{where}: "{text}": {"expected " + expected if expected else "ends too soon"}')
        position[0] += 1
        return token

    def either():
        left = both()
        while peek() == '||':
            take()
            left = ('or', left, both())
        return left

    def both():
        left = unary()
        while peek() == '&&':
            take()
            left = ('and', left, unary())
        return left

    def unary():
        if peek() == '!':
            take()
            return ('not', unary())
        if peek() == '(':
            take()
            inner = either()
            take(')')
            return inner
        token = take()
        if not re.fullmatch(r'[A-Za-z0-9_]+', token):
            raise KconfigError(f'{where}: "{text}": a symbol was expected, not {token}')
        if peek() in ('=', '!='):
            operator = take()
            other = take().strip('"')
            if not re.fullmatch(r'[A-Za-z0-9_]+', other):
                raise KconfigError(f'{where}: "{text}": a symbol or y or n was expected after {operator}, not {other}')
            # (as Linux: both sides are values, a symbol's or the constants y and n)
            return ('eq' if operator == '=' else 'ne', token, other)
        return ('symbol', token)

    tree = either()
    if peek() is not None:
        raise KconfigError(f'{where}: "{text}": {peek()} is left over')
    return tree


def symbols_in(tree):
    kind = tree[0]
    if kind == 'symbol':
        return {tree[1]} - {'y', 'n'}
    if kind in ('eq', 'ne'):
        return {tree[1], tree[2]} - {'y', 'n'}
    if kind == 'not':
        return symbols_in(tree[1])
    return symbols_in(tree[1]) | symbols_in(tree[2])


def evaluate(tree, values):
    kind = tree[0]
    if kind == 'symbol':
        name = tree[1]
        return name == 'y' or (name != 'n' and values.get(name, False))
    if kind in ('eq', 'ne'):
        same = evaluate(('symbol', tree[1]), values) == evaluate(('symbol', tree[2]), values)
        return same == (kind == 'eq')
    if kind == 'not':
        return not evaluate(tree[1], values)
    if kind == 'and':
        return evaluate(tree[1], values) and evaluate(tree[2], values)
    return evaluate(tree[1], values) or evaluate(tree[2], values)


# ---- the files ---------------------------------------------------------------------------------------------------

class Kconfig:
    def __init__(self, root=ROOT, top=TOP_KCONFIG):
        self.root = Path(root)
        self.symbols = {}               # name -> Symbol, in the order they are described
        self.files = []
        self.title = ''
        self.problems = []
        self._read(top, [], [])

    def _read(self, name, conditions, menus):
        path = self.root / name
        if not path.is_file():
            raise KconfigError(f'no Kconfig file {name}')
        self.files.append(name)
        lines = path.read_text().split('\n')
        index = 0
        current = None
        stack = []                      # ('menu', title, conditions) or ('if', conditions)
        while index < len(lines):
            raw = lines[index]
            where = f'{name}:{index + 1}'
            line = raw.split('#', 1)[0].rstrip() if not raw.lstrip().startswith('help') else raw.rstrip()
            stripped = line.strip()
            index += 1
            if not stripped:
                continue
            word, _, rest = stripped.partition(' ')
            rest = rest.strip()
            if word in ('config', 'menuconfig'):
                if not re.fullmatch(r'[A-Z0-9_]+', rest):
                    raise KconfigError(f'{where}: a symbol is upper case, digits and _: {rest}')
                if rest in self.symbols:
                    raise KconfigError(f'{where}: {rest} is described already, at {":".join(map(str, self.symbols[rest].where))}')
                current = Symbol(rest, (name, index))
                current.depends = list(conditions)
                current.menu = menus[-1] if menus else ''
                self.symbols[rest] = current
                continue
            if word in ('bool', 'prompt', 'tristate', 'int', 'hex', 'string'):
                if word in ('tristate', 'int', 'hex', 'string'):
                    raise KconfigError(f'{where}: only bool is known here (the kernel has no modules): {word}')
                if current is None:
                    raise KconfigError(f'{where}: {word} outside a config entry')
                if not rest and word == 'bool':
                    continue            # (no prompt: a symbol nobody chooses, on when something selects it)
                prompt, condition = self._quoted(rest, where)
                current.prompt = prompt
                if condition is not None:
                    # (as Linux: it hides the prompt, not the symbol -- hidden, it takes its default)
                    current.visible.append(condition)
                continue
            if word == 'def_bool':
                if current is None:
                    raise KconfigError(f'{where}: def_bool outside a config entry')
                value, _, condition = rest.partition(' if ')
                current.defaults.append((parse_expression(value, where), parse_expression(condition, where) if condition else None))
                continue
            if word == 'default':
                if current is None:
                    raise KconfigError(f'{where}: default outside a config entry')
                value, _, condition = rest.partition(' if ')
                current.defaults.append((parse_expression(value, where), parse_expression(condition, where) if condition else None))
                continue
            if word == 'depends':
                if current is None or not rest.startswith('on '):
                    raise KconfigError(f'{where}: "depends on" belongs to a config entry')
                current.depends.append(parse_expression(rest[3:], where))
                continue
            if word == 'select':
                if current is None:
                    raise KconfigError(f'{where}: select outside a config entry')
                target, _, condition = rest.partition(' if ')
                current.selects.append((target.strip(), parse_expression(condition, where) if condition else None))
                continue
            if word in ('help', '---help---'):
                if current is None:
                    raise KconfigError(f'{where}: help outside a config entry')
                text = []
                indent = None
                while index < len(lines):
                    body = lines[index]
                    if body.strip() == '':
                        text.append('')
                        index += 1
                        continue
                    expanded = body.expandtabs(8)       # (as Linux measures it: a tab to the next column of eight)
                    width = len(expanded) - len(expanded.lstrip())
                    if indent is None:
                        indent = width
                    if width < indent or width == 0:
                        break
                    text.append(body.strip())
                    index += 1
                current.help = ' '.join(t for t in text if t).strip()
                continue
            current = None
            if word == 'mainmenu':
                self.title, _ = self._quoted(rest, where)
            elif word == 'source':
                source, _ = self._quoted(rest, where)
                self._read(source, conditions, menus)
            elif word == 'menu':
                title, _ = self._quoted(rest, where)
                stack.append(('menu', title, conditions))
                menus = menus + [title]
            elif word == 'endmenu':
                if not stack or stack[-1][0] != 'menu':
                    raise KconfigError(f'{where}: endmenu without its menu')
                _, _, conditions = stack.pop()
                menus = menus[:-1]
            elif word == 'if':
                stack.append(('if', conditions))
                conditions = conditions + [parse_expression(rest, where)]
            elif word == 'endif':
                if not stack or stack[-1][0] != 'if':
                    raise KconfigError(f'{where}: endif without its if')
                _, conditions = stack.pop()
            elif word == 'comment':
                self._quoted(rest, where)
            else:
                raise KconfigError(f'{where}: {word} is not a word of the language this tree uses')
        if stack:
            raise KconfigError(f'{name}: {stack[-1][0]} not closed at the end of the file')

    @staticmethod
    def _quoted(text, where):
        match = re.fullmatch(r'"([^"]*)"(?:\s+if\s+(.+))?', text)
        if not match:
            raise KconfigError(f'{where}: a quoted text was expected: {text}')
        return match.group(1), (parse_expression(match.group(2), where) if match.group(2) else None)

    # ---- checks ----

    def check(self, used=()):
        problems = []
        for symbol in self.symbols.values():
            where = ':'.join(map(str, symbol.where))
            for expression in symbol.depends + [d for d, _ in symbol.defaults] + [c for _, c in symbol.defaults if c] + \
                    [c for _, c in symbol.selects if c]:
                for name in symbols_in(expression):
                    if name not in self.symbols:
                        problems.append(f'{where}: {symbol.name} depends on or defaults to {name}, which no Kconfig describes')
            for target, _ in symbol.selects:
                if target not in self.symbols:
                    problems.append(f'{where}: {symbol.name} selects {target}, which no Kconfig describes')
            if symbol.prompt is not None and not symbol.help:
                problems.append(f'{where}: {symbol.name} can be chosen and says nothing of what it is (help)')
        for name, where in used:
            if name not in self.symbols:
                problems.append(f'{where}: CONFIG_{name} is used, and no Kconfig describes {name}')
        return problems

    # ---- a configuration ----

    def resolve(self, asked):
        """Every symbol's value: what the fragments ask where the dependencies allow it, else the default; selected ones y.
        Returns (values, warnings)."""
        warnings = []
        for name in asked:
            if name not in self.symbols:
                warnings.append(f'CONFIG_{name}: no Kconfig describes it: left out')
        values = {name: False for name in self.symbols}
        for _ in range(len(self.symbols) + 2):
            changed = False
            selected = set()
            for symbol in self.symbols.values():
                if values[symbol.name]:
                    for target, condition in symbol.selects:
                        if target in self.symbols and (condition is None or evaluate(condition, values)):
                            selected.add(target)
            for symbol in self.symbols.values():
                allowed = all(evaluate(d, values) for d in symbol.depends)
                if symbol.name in selected:
                    value = True
                elif symbol.prompt is not None and symbol.name in asked and all(evaluate(v, values) for v in symbol.visible):
                    value = asked[symbol.name] and allowed
                else:
                    value = False
                    for default, condition in symbol.defaults:
                        if condition is None or evaluate(condition, values):
                            value = evaluate(default, values)
                            break
                    value = value and allowed
                if value != values[symbol.name]:
                    values[symbol.name] = value
                    changed = True
            if not changed:
                break
        else:
            # (Linux refuses such a tree as a recursive dependency: here what it came to is said not to be settled)
            warnings.append('the configuration does not settle: a select and a dependency undo each other (a loop); '
                            'what is written is where it stopped')
        for symbol in self.symbols.values():
            if values[symbol.name] and not all(evaluate(d, values) for d in symbol.depends):
                warnings.append(f'CONFIG_{symbol.name} is selected, and its own dependencies are not met')
            if symbol.name in asked and asked[symbol.name] and not values[symbol.name]:
                warnings.append(f'CONFIG_{symbol.name}=y asked for, and its dependencies are not met: n')
        return values, warnings

    def defaults(self):
        return self.resolve({})[0]

    def default_under(self, symbol, values):
        """what SYMBOL would be under VALUES (every other symbol as there) if nobody asked: its first default that holds,
        within its dependencies"""
        for default, condition in symbol.defaults:
            if condition is None or evaluate(condition, values):
                return evaluate(default, values) and all(evaluate(d, values) for d in symbol.depends)
        return False

    def selected_under(self, name, values):
        return any(values[s.name] and target == name and (condition is None or evaluate(condition, values))
                   for s in self.symbols.values() for target, condition in s.selects)

    def savedefconfig(self, values):
        """VALUES as the smallest fragment Linux's savedefconfig would write: each choice whose value is not what its default
        would be under VALUES (not under no fragment at all), selected ones left to their selection; then checked to give
        VALUES back, and whatever it does not, written too. Returns the fragment's lines."""
        written = {}
        for symbol in self.symbols.values():
            if symbol.prompt is None or self.selected_under(symbol.name, values):
                continue
            if values[symbol.name] != self.default_under(symbol, values):
                written[symbol.name] = values[symbol.name]
        for _ in range(len(self.symbols)):
            again = self.resolve(written)[0]
            missing = [s.name for s in self.symbols.values() if s.prompt is not None and s.name not in written and
                       again[s.name] != values[s.name]]
            if not missing:
                break
            written[missing[0]] = values[missing[0]]
        return [f'CONFIG_{name}=y' if written[name] else f'# CONFIG_{name} is not set'
                for name in self.symbols if name in written]


def read_fragment(path):
    asked = {}
    for number, line in enumerate(Path(path).read_text().split('\n'), 1):
        line = line.strip()
        match = re.fullmatch(r'CONFIG_([A-Z0-9_]+)=(y|n)', line) or re.fullmatch(r'# CONFIG_([A-Z0-9_]+) is not set()', line)
        if match:
            asked[match.group(1)] = match.group(2) == 'y'
        elif line and not line.startswith('#'):
            raise KconfigError(f'{path}:{number}: a fragment has CONFIG_NAME=y, CONFIG_NAME=n or "# CONFIG_NAME is not set": {line}')
    return asked


def fragment_path(name, root=ROOT):
    path = Path(name)
    if path.is_file():
        return path
    for candidate in (root / 'arch/arm64/configs/kconfig' / name, root / 'arch/arm64/configs/kconfig' / f'{name}.config'):
        if candidate.is_file():
            return candidate
    raise KconfigError(f'no fragment {name} (arch/arm64/configs/kconfig/: {", ".join(sorted(p.stem for p in (root / "arch/arm64/configs/kconfig").glob("*.config")))})')


def configuration(kconfig, fragments, root=ROOT):
    if not fragments:
        fragments = os.environ.get('KCONFIG_FRAGMENTS', '').split()
    asked = {}
    paths = [fragment_path(f, root) for f in fragments]
    for path in paths:
        asked.update(read_fragment(path))
    values, warnings = kconfig.resolve(asked)
    return values, warnings, paths


def dotconfig(kconfig, values, paths):
    lines = [f'# the kernel\'s configuration, made by tools/kconfig.py from {kconfig.files[0]}'
             f'{" and " + ", ".join(str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p) for p in paths) if paths else " (every default)"}']
    menu = None
    for symbol in kconfig.symbols.values():
        if symbol.menu != menu:
            menu = symbol.menu
            lines += ['', f'# {menu}' if menu else '#']
        lines.append(f'CONFIG_{symbol.name}=y' if values[symbol.name] else f'# CONFIG_{symbol.name} is not set')
    return '\n'.join(lines) + '\n'


def header(kconfig, values, paths):
    names = ', '.join(p.name for p in paths) if paths else 'every default'
    lines = ['/* GENERATED by tools/kconfig.py (tools/gen_headers.py) from Kconfig and ' + names + ': do not edit. */',
             '/* Every symbol is defined, 1 or 0, so that the code asks if (CONFIG_NAME): what is off is compiled all the same */',
             '/* and never started. Each is under #ifndef: a test sets one first. */',
             '#ifndef HAKIOS_KCONFIG_H', '#define HAKIOS_KCONFIG_H']
    for symbol in kconfig.symbols.values():
        lines += [f'#ifndef CONFIG_{symbol.name}', f'#define CONFIG_{symbol.name} {1 if values[symbol.name] else 0}', '#endif']
    off = ' '.join(s.name for s in kconfig.symbols.values() if s.prompt is not None and not values[s.name])
    lines += ['/* the choices this kernel was made without, and the fragments it was made of: said in the log at boot */',
              '#ifndef KCONFIG_OFF', f'#define KCONFIG_OFF "{off}"', '#endif',
              '#ifndef KCONFIG_MADE_OF', f'#define KCONFIG_MADE_OF "{names}"', '#endif', '#endif', '']
    return '\n'.join(lines)


def used_symbols(root=ROOT):
    """(name, file:line) of every CONFIG_NAME asked as the configuration is -- if (CONFIG_NAME), C's own if: XNU's own
    options are #if's -- in the files of ours that read kconfig.h (drivers/, arch/arm64/kernel/)"""
    found = []
    for directory in ('drivers', 'arch/arm64/kernel'):
        for path in sorted((root / directory).rglob('*')):
            if path.suffix not in ('.c', '.cpp', '.h') or not path.is_file():
                continue
            text = path.read_text(errors='replace')
            if '"kconfig.h"' not in text:
                continue
            for number, line in enumerate(text.split('\n'), 1):
                for name in re.findall(r'\bif \(CONFIG_([A-Z][A-Z0-9_]*)\)', line):
                    found.append((name, f'{path.relative_to(root)}:{number}'))
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('check')
    for name in ('list', 'config', 'header', 'savedefconfig'):
        command = commands.add_parser(name)
        command.add_argument('fragments', nargs='*')
        if name in ('config', 'header'):
            command.add_argument('--out')
    options = parser.parse_args(argv)
    try:
        kconfig = Kconfig()
        if options.command == 'check':
            problems = kconfig.check(used_symbols())
            for fragment in sorted(FRAGMENTS.glob('*.config')):
                try:
                    asked = read_fragment(fragment)
                except KconfigError as error:
                    problems.append(str(error))
                    continue
                _, warnings = kconfig.resolve(asked)
                problems += [f'{fragment.relative_to(ROOT)}: {w}' for w in warnings]
            for problem in problems:
                print(problem, file=sys.stderr)
            print(f'kconfig: {len(kconfig.files)} files, {len(kconfig.symbols)} symbols, '
                  f'{len(list(FRAGMENTS.glob("*.config")))} fragments: {"ok" if not problems else f"{len(problems)} problems"}')
            return 1 if problems else 0
        values, warnings, paths = configuration(kconfig, options.fragments)
        for warning in warnings:
            print(f'kconfig: {warning}', file=sys.stderr)
        if options.command == 'list':
            for symbol in kconfig.symbols.values():
                what = symbol.prompt or ('(not a choice)' if symbol.defaults else '(on when a driver that uses it is)')
                print(f'{"y" if values[symbol.name] else "n"}  {symbol.name:28} {what}')
        elif options.command == 'savedefconfig':
            for line in kconfig.savedefconfig(values):
                print(line)
        else:
            text = dotconfig(kconfig, values, paths) if options.command == 'config' else header(kconfig, values, paths)
            out = Path(options.out or ROOT / ('build/kconfig/.config' if options.command == 'config' else 'build/boards/kconfig.h'))
            out.parent.mkdir(parents=True, exist_ok=True)
            if not out.is_file() or out.read_text() != text:
                out.write_text(text)
            print(f'kconfig: {out}: {sum(values.values())} of {len(values)} symbols on')
        return 0
    except KconfigError as error:
        print(f'kconfig: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
