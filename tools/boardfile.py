"""The files that describe a phone and its SoC: arch/arm64/boot/dts/NAME/board.dts and soc.dtsi.

They are written in the syntax of a device tree source, the one a porter reads in the phone's Android
tree, with a small part of it:

    /dts-v1/;

    // a comment to the end of the line; /* ... */ as well
    / {
        name = "phone";                         // a string
        features = "cpufreq-epss", "adsp";      // several
        memory = <0x9d400000>;                  // a number, in cells: decimal or 0x hexadecimal
        bus-pins = <52 53 54 55>;               // several
        fb-base = /bits/ 64 <0x9c000000>;       // an address of 64 bits
        uefi-charger;                           // a flag: there is true, absent is false

        speaker {                               // a section
            sda = <12>;
        };
        camera {
            sensor@0 {                          // one of several tables of the same kind
                name = "wide";
            };
        };
    };

A file is the root node / and, inside it, one node a section (the root may be opened again lower in the
file, as dtc allows: the properties and sections of each are the file's); a section's nodes NAME@N are the tables
of its key NAMEs (camera's sensor@0, sensor@1, ... are its "sensors"). Property and node names are
written with dashes and are the schema's keys with underscores (tools/boards.py: BOARD_SCHEMA,
SOC_SCHEMA). Anything else of a device tree's syntax -- labels, references, includes, deletions -- is
refused with the line it is on: these files are read by a tool, not compiled.

parse() returns the file as tools/boards.py checks it: {key: value, section: {key: value}}, each value
of the type its key has in the schema; a value that is not what its key wants is given back as it was
written, for the check to say so.
"""
import re


class FileError(Exception):
    """What is wrong with the syntax of a file, with the line it is on."""

    def __init__(self, line, message):
        super().__init__(f'line {line}: {message}')
        self.line = line


class Value:
    """A property as written: strings, numbers (cells), a 64-bit number, or a flag."""

    def __init__(self, kind, items, line):
        self.kind, self.items, self.line = kind, items, line   # kind: 'strings', 'cells', 'bits64', 'flag'

    def plain(self):
        """What it is without a schema: one item alone, several as a list, a flag as True."""
        if self.kind == 'flag':
            return True
        return self.items[0] if len(self.items) == 1 else list(self.items)


TOKEN = re.compile(r'''
    (?P<space>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<cells><[^<>]*>)
  | (?P<bits>/bits/)
  | (?P<header>/dts-v1/)
  | (?P<word>[A-Za-z0-9_][A-Za-z0-9_,.+@-]*|/)
  | (?P<punct>[{};=,])
''', re.X | re.S)
NUMBER = re.compile(r'0[xX][0-9a-fA-F]+|[0-9]+')


def tokens(text):
    position, line = 0, 1
    while position < len(text):
        match = TOKEN.match(text, position)
        if match is None:
            raise FileError(line, f'{text[position:position + 20].split()[0]!r} is not of this format: '
                                  'strings in "", numbers in <>, a property ends with ;')
        kind = match.lastgroup
        if kind not in ('space', 'comment'):
            yield kind, match.group(), line
        line += match.group().count('\n')
        position = match.end()
    yield 'end', '', line


def unquote(text):
    return re.sub(r'\\(.)', r'\1', text[1:-1])


def numbers(text, line):
    found = text[1:-1].split()
    values = []
    for item in found:
        if not NUMBER.fullmatch(item):
            raise FileError(line, f'<{text[1:-1]}>: only numbers in <> (decimal or 0x...), no {item!r}')
        if item[:2] in ('0x', '0X'):
            values.append(int(item, 16))
        elif len(item) > 1 and item[0] == '0':
            # (as dtc reads it, and C: a leading 0 is octal -- 010 is 8, and 08 is no number at all)
            if not re.fullmatch(r'[0-7]+', item):
                raise FileError(line, f'<{text[1:-1]}>: {item!r} begins with 0, which makes it octal, and is not')
            values.append(int(item, 8))
        else:
            values.append(int(item, 10))
    return values


def read_tree(text):
    """The file as nodes: {'props': {name: Value}, 'nodes': [(name, unit or None, node, line)]}."""
    stream = tokens(text)

    def take():
        return next(stream)

    def expect(kind, text_=None, what=''):
        token = take()
        if token[0] != kind or (text_ is not None and token[1] != text_):
            raise FileError(token[2], f'{what or text_ or kind} expected, not {token[1] or "the end of the file"!r}')
        return token

    def node_body(depth):
        node = {'props': {}, 'nodes': []}
        while True:
            token = take()
            if token[0] == 'punct' and token[1] == '}':
                expect('punct', ';', '"};" (a node ends with a semicolon)')
                return node
            if token[0] != 'word' or token[1] == '/':
                raise FileError(token[2], f'a property or a node expected, not {token[1] or "the end of the file"!r}')
            name, line = token[1], token[2]
            after = take()
            if after[0] == 'punct' and after[1] == '{':
                if depth >= 2:
                    raise FileError(line, f'{name}: nodes go two deep at most (a section, and its tables)')
                base, _, unit = name.partition('@')
                if unit and not re.fullmatch(r'[0-9]+', unit):
                    raise FileError(line, f'{name}: a table is NAME@N, N its place from 0')
                node['nodes'].append((base, int(unit) if unit else None, node_body(depth + 1), line))
                continue
            if '@' in name:
                raise FileError(line, f'{name}: only a node has an @')
            if name in node['props']:
                raise FileError(line, f'{name} is given twice')
            if after[0] == 'punct' and after[1] == ';':
                node['props'][name] = Value('flag', [], line)
                continue
            if not (after[0] == 'punct' and after[1] == '='):
                raise FileError(after[2], f'{name}: "=" or ";" expected, not {after[1]!r}')
            node['props'][name] = value(line, name)

    def value(line, name):
        strings, cells, bits = [], [], []
        while True:
            token = take()
            if token[0] == 'string':
                strings.append(unquote(token[1]))
            elif token[0] == 'cells':
                cells.extend(numbers(token[1], token[2]))
            elif token[0] == 'bits':
                expect('word', '64', '64 (/bits/ 64 <...>: an address of 64 bits)')
                bits.extend(numbers(expect('cells', what='<...>')[1], token[2]))
            else:
                raise FileError(token[2], f'{name}: a value expected ("...", <...> or /bits/ 64 <...>), not {token[1]!r}')
            token = take()
            if token[0] == 'punct' and token[1] == ';':
                break
            if not (token[0] == 'punct' and token[1] == ','):
                raise FileError(token[2], f'{name}: "," or ";" expected after a value, not {token[1] or "the end"!r}')
        if sum(1 for kind in (strings, cells, bits) if kind) > 1:
            raise FileError(line, f'{name}: strings and numbers in one property')
        if strings:
            return Value('strings', strings, line)
        return Value('bits64', bits, line) if bits else Value('cells', cells, line)

    token = take()
    if token[0] == 'header':
        expect('punct', ';', '";" after /dts-v1/')
        token = take()
    if token[:2] != ('word', '/'):
        raise FileError(token[2], 'the file is the root node: / { ... };')
    root = {'props': {}, 'nodes': []}
    # (the root may be opened again further down, as dtc allows: what that block has is added to it)
    while token[:2] == ('word', '/'):
        expect('punct', '{', '"{" after /')
        more = node_body(0)
        for name, value_ in more['props'].items():
            if name in root['props']:
                raise FileError(value_.line, f'{name} is given twice')
            root['props'][name] = value_
        root['nodes'] += more['nodes']
        token = take()
    if token[0] != 'end':
        raise FileError(token[2], f'nothing but the root node / {{ ... }}; in the file, not {token[1]!r}')
    return root


def key_of(name):
    return name.replace('-', '_')


def typed(value, kind):
    """A value as its key's schema type wants it: a flag is True, a list's key gets a list even of one, any
    other one value alone. What is not of the key's type comes out as written, for the check to say so."""
    if value.kind == 'flag':
        return True
    if isinstance(kind, list) and not isinstance(kind[0], dict):
        return list(value.items)
    return value.plain()


def parse(text, schema):
    """The file as a dictionary of the schema's keys. Raises FileError on what is not this syntax."""
    root = read_tree(text)

    def table(node, keys, where):
        out = {}
        for name, value in node['props'].items():
            key = key_of(name)
            out[key] = typed(value, keys[key][0]) if key in keys else value.plain()
        groups = {}
        for name, unit, child, line in node['nodes']:
            if unit is None:
                raise FileError(line, f'{name} {{ inside {where}: a table is NAME@N')
            groups.setdefault(key_of(name), []).append((unit, child, line))
        for name, items in groups.items():
            key = name + 's'
            kind = keys.get(key, (None,))[0]
            if not (isinstance(kind, list) and isinstance(kind[0], dict)):
                raise FileError(items[0][2], f'{name}@N: {where} has no tables of that name')
            items.sort(key=lambda item: item[0])
            if [unit for unit, _, _ in items] != list(range(len(items))):
                raise FileError(items[0][2], f'the {name}@N of {where} are numbered 0, 1, 2, ... in order')
            out[key] = [table(child, kind[0], f'{name}@{unit}') for unit, child, _ in items]
        return out

    data = {}
    for name, value in root['props'].items():
        key = key_of(name)
        data[key] = typed(value, schema[''][key][0]) if key in schema[''] else value.plain()
    for name, unit, node, line in root['nodes']:
        section = key_of(name)
        if unit is not None:
            raise FileError(line, f'{name}@{unit}: a section is a node without @')
        if section in data:
            raise FileError(line, f'{name} is given twice')
        data[section] = table(node, schema.get(section, {}), name) if section in schema else \
            {key_of(k): v.plain() for k, v in node['props'].items()}
    return data


# ---- writing ---------------------------------------------------------------------------------------

def name_of(key):
    return key.replace('_', '-')


def value_text(value, number=None):
    """A value in this syntax. number(value) writes a number (hexadecimal or not); the default: decimal
    up to 4096, hexadecimal above."""
    number = number or (lambda v: f'{v:#x}' if v > 4096 else str(v))
    if isinstance(value, str):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    if isinstance(value, int) and value > 0xffffffff:
        return f'/bits/ 64 <{value:#x}>'
    if isinstance(value, int):
        return f'<{number(value)}>'
    if value and all(isinstance(v, str) for v in value):
        return ', '.join(value_text(v) for v in value)
    return '<' + ' '.join(number(v) for v in value) + '>'


def property_line(key, value, number=None):
    """key = value; as a line (a flag: key; -- and nothing when it is false)."""
    if value is True:
        return f'{name_of(key)};'
    if value is False:
        return ''
    return f'{name_of(key)} = {value_text(value, number)};'


# ---- from the files of before ----------------------------------------------------------------------

def _toml_value(text):
    """A TOML value as this syntax writes it, keeping how each number was written (0x0ad00000 stays so)."""
    import tomllib
    value = tomllib.loads('v = ' + text)['v']
    if isinstance(value, bool):
        return None if not value else True
    written = re.findall(r'0[xX][0-9a-fA-F_]+|[0-9][0-9_]*', re.sub(r'"(?:[^"\\]|\\.)*"', '', text))
    if isinstance(value, int):
        return f'/bits/ 64 <{written[0]}>' if value > 0xffffffff else f'<{written[0].replace("_", "")}>'
    if isinstance(value, str):
        return value_text(value)
    if value and all(isinstance(v, str) for v in value):
        return ', '.join(value_text(v) for v in value)
    return '<' + ' '.join(w.replace('_', '') for w in written) + '>'


def _split_comment(line):
    """(code, comment) of a TOML line: the # that is not inside a string."""
    quoted = False
    for i, c in enumerate(line):
        if c == '"' and (i == 0 or line[i - 1] != '\\'):
            quoted = not quoted
        elif c == '#' and not quoted:
            return line[:i].rstrip(), line[i + 1:]
    return line.rstrip(), None


def from_toml(text):
    """A board.toml or SOC.toml of before as this format, with its comments where they were. Keys that
    were commented out (the templates' "# key = value") stay commented out, in the new syntax."""
    out = []                    # (indent, text, is_content)
    open_blocks = []            # ('real' | 'commented', depth)
    table_counts = {}

    def emit(depth, text_, content=False):
        out.append((depth, text_, content))

    def close_to(depth):
        # close the blocks deeper than depth, right after the last line of content in them
        while open_blocks and open_blocks[-1][1] >= depth:
            kind, at = open_blocks.pop()
            where = len(out)
            while where > 0 and not out[where - 1][2]:
                where -= 1
            out.insert(where, (at, '};' if kind == 'real' else '// };', True))

    def statement(key, value_text_, comment, commented, depth):
        line = f'{name_of(key)} = {value_text_};' if value_text_ is not True else f'{name_of(key)};'
        if commented:
            line = '// ' + line
        if comment is not None:
            line += '\0//' + (comment if comment.startswith(' ') else ' ' + comment)     # (aligned at the end)
        emit(depth, line, True)

    lines = text.split('\n')
    i, depth, root_open = 0, 1, False
    while i < len(lines):
        raw = lines[i]
        stripped = raw.strip()
        i += 1
        if not stripped:
            emit(0, '')
            continue
        commented = stripped.startswith('#')
        body = stripped[1:].strip() if commented else stripped
        if commented and (raw[:1].isspace() or body.startswith('#')):
            # the comment after a value, going on under it (as "# key = value  # ..." of a template does too)
            rest = body[1:].strip() if body.startswith('#') else body
            emit(depth, ('//\0// ' if body.startswith('#') else '\0// ') + rest, True)
            continue
        header = re.fullmatch(r'\[(\[?)([\w.]+)\]?\]', body.split('#')[0].strip()) if body.startswith('[') else None
        keyed = re.match(r'([A-Za-z_][\w-]*) = (.*)$', body)
        if header:
            if not root_open:
                emit(0, '/ {', True)
                root_open = True
            name = header[2]
            if header[1]:                       # [[section.key]]: a table of the section
                section, key = name.split('.')
                close_to(2)
                unit = table_counts.get((section, key, commented), 0)
                table_counts[(section, key, commented)] = unit + 1
                emit(2, ('// ' if commented else '') + f'{name_of(key[:-1] if key.endswith("s") else key)}@{unit} {{', True)
                open_blocks.append(('commented' if commented else 'real', 2))
                depth = 3
            else:
                close_to(1)
                emit(1, ('// ' if commented else '') + f'{name_of(name)} {{', True)
                open_blocks.append(('commented' if commented else 'real', 1))
                depth = 2
            # the comments just above the header belong to it: at its level
            j = len(out) - 2
            while j >= 0 and not out[j][2] and out[j][1]:
                out[j] = (depth - 1, out[j][1], False)
                j -= 1
            continue
        if keyed:
            code, comment = _split_comment(keyed[2]) if not commented else _split_comment(keyed[2])
            while code.count('[') > code.count(']') and i < len(lines):  # an array over several lines
                more, more_comment = _split_comment(lines[i].strip())
                code += ' ' + more
                i += 1
            try:
                value = _toml_value(code)
            except Exception:
                value = f'<{code}>' if code.startswith('0x') else code     # (a template's "0x..." placeholder)
            if value is None:
                continue                        # false: not there
            if not root_open:
                emit(0, '/ {', True)
                root_open = True
            statement(keyed[1], value, comment, commented, depth)
            continue
        # a comment of its own
        emit(depth if root_open else 0, '//' + (body if not body or body.startswith(' ') else ' ' + body) if commented else stripped)
    open_blocks.insert(0, ('real', 0))
    close_to(0)
    # the indentation: tabs, a level each; a comment after a value at the same column as its neighbours'
    lines_out = []
    for d, t, _ in out:
        line = ('\t' * d + t) if t else ''
        if '\0' in line:
            code, comment = line.split('\0')
            width = len(code.expandtabs(8))
            line = code + ' ' * max(1, COMMENT_COLUMN - width) + comment
        lines_out.append(line)
    text_out = '/dts-v1/;\n\n' + '\n'.join(lines_out).rstrip() + '\n'
    return re.sub(r'\n{3,}', '\n\n', text_out)


COMMENT_COLUMN = 48     # where a comment after a value begins (tabs counted as 8)
