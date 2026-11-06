#!/usr/bin/env python3
"""
Print the nodes of a flattened device tree whose path, compatible or properties match a word.

  fdtfind.py <file.dtb> <word> [...]

For looking at the phone's own Android device tree without the phone in hand (prebuilt/uefi).
"""
import struct
import sys

FDT_BEGIN, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse(d):
    magic, total, off_struct, off_strings = struct.unpack_from('>4I', d, 0)
    assert magic == 0xd00dfeed, 'not a flattened device tree'
    strings = d[off_strings:]
    at, path, out = off_struct, [], []
    while True:
        tag, = struct.unpack_from('>I', d, at)
        at += 4
        if tag == FDT_BEGIN:
            end = d.index(b'\0', at)
            path.append(d[at:end].decode())
            out.append(('/'.join(path), {}))
            at = (end + 4) & ~3
        elif tag == FDT_END_NODE:
            path.pop()
        elif tag == FDT_PROP:
            ln, noff = struct.unpack_from('>II', d, at)
            at += 8
            name = strings[noff:strings.index(b'\0', noff)].decode()
            out[-1][1][name] = d[at:at + ln]
            at = (at + ln + 3) & ~3
        elif tag == FDT_NOP:
            pass
        elif tag == FDT_END:
            return out


def show(value):
    if value and all(32 <= c < 127 or c == 0 for c in value) and value[-1:] == b'\0':
        return ' '.join(s.decode() for s in value.split(b'\0') if s)
    if len(value) % 4 == 0 and len(value) <= 32:
        return ' '.join(f'{v:#x}' for v in struct.unpack(f'>{len(value) // 4}I', value))
    return value[:32].hex() + ('…' if len(value) > 32 else '')


def main():
    d = open(sys.argv[1], 'rb').read()
    words = [w.lower() for w in sys.argv[2:]]
    for path, props in parse(d):
        hay = (path + ' ' + ' '.join(props) + ' ' + show(props.get('compatible', b''))).lower()
        if any(w in hay for w in words):
            print(f'{path}')
            for name, value in props.items():
                print(f'    {name} = {show(value)}')


if __name__ == '__main__':
    main()
