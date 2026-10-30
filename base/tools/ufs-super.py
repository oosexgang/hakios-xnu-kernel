#!/usr/bin/env python3
"""What is in the phone's "super" partition, and where in it nothing is.

usage: ufs-super.py [LOG]
  LOG  the loader's log of a run with ufs=1 among XL_CMDLINE (default logs/last.log)

Android's system, vendor, product and the rest are logical partitions inside the GPT partition
"super", laid out by liblp's metadata at its start. With ufs=1 the loader (xnuloader/loader.c,
ufs_dump_super) prints that metadata raw: the geometry, the header of the first slot and its tables.
This checks their SHA-256 sums -- right sums mean the bytes are the metadata as Android wrote it --
lists the logical partitions with their extents and groups, and says which ranges of super no
extent covers. Nothing is read from the phone here; it only reads the log.
"""
import hashlib
import re
import struct
import sys

SECTOR = 512


def blob(text, what):
    rows = re.findall(rf'^ufs: super {what} \+([0-9a-f]+): ([0-9a-f]*)$', text, re.M)
    data = bytearray()
    for at, hexa in rows:
        if int(at, 16) != len(data):
            sys.exit(f'ufs-super.py: the {what} lines are not contiguous at +{at}')
        data += bytes.fromhex(hexa)
    return bytes(data)


def checked(data, at, name):
    """the sum at data[at:at+32] over data with that field zeroed"""
    zeroed = data[:at] + bytes(32) + data[at + 32:]
    right = hashlib.sha256(zeroed).digest() == data[at:at + 32]
    print(f'{name} sha256 {"right" if right else "WRONG"}')
    return right


def cstr(b):
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


def size(n):
    return f'{n / 2**30:.2f} GB' if n >= 2**30 else f'{n / 2**20:.1f} MB'


def slot(number, header, tables):
    print(f'\n=== metadata slot {number} (as slot _{"ab"[number]} sees super) ===')
    _, major, minor, header_size = struct.unpack_from('<IHHI', header)
    tables_size = struct.unpack_from('<I', header, 44)[0]
    ok = checked(header[:header_size], 12, f'header v{major}.{minor}')
    right = hashlib.sha256(tables).digest() == header[48:80]
    print(f'tables sha256 {"right" if right else "WRONG"} ({tables_size} bytes)')
    ok &= right
    desc = [struct.unpack_from('<III', header, 80 + 12 * i) for i in range(4)]
    flags = struct.unpack_from('<I', header, 128)[0] if header_size >= 132 else 0

    def table(i, fmt):
        offset, count, entry = desc[i]
        return [struct.unpack_from(fmt, tables, offset + k * entry) for k in range(count)]

    partitions = table(0, '<36sIIII')
    extents = table(1, '<QIQI')
    groups = table(2, '<36sIQ')
    devices = table(3, '<QIIQ36sI')

    print(f'block devices (header flags {flags:#x}):')
    for first, align, align_off, dev_size, name, dflags in devices:
        print(f'  {cstr(name)}: {size(dev_size)} ({dev_size} bytes), data from sector {first}, '
              f'alignment {align}, flags {dflags:#x}')
    print('groups:')
    used_in = {}
    for _, attrs, first, count, group in partitions:
        used_in[group] = used_in.get(group, 0) + sum(extents[first + k][0] for k in range(count)) * SECTOR
    for i, (name, gflags, maximum) in enumerate(groups):
        room = f', {size(maximum - used_in.get(i, 0))} of it unused' if maximum else ''
        print(f'  {cstr(name) or "(default)"}: at most {size(maximum) if maximum else "unlimited"}, '
              f'holds {size(used_in.get(i, 0))}{room}')

    print('logical partitions:')
    taken = []
    for name, attrs, first, count, group in partitions:
        parts = []
        for k in range(count):
            sectors, kind, target, source = extents[first + k]
            if kind == 0:
                taken.append((target, target + sectors, cstr(name), source))
                parts.append(f'{sectors * SECTOR / 2**20:.0f} MB at sector {target}')
            else:
                parts.append(f'{sectors * SECTOR / 2**20:.0f} MB of zeros')
        total = sum(extents[first + k][0] for k in range(count)) * SECTOR
        print(f'  {cstr(name):<20} {size(total):>9}  group {cstr(groups[group][0]) or group}, '
              f'attributes {attrs:#x}' + (f': {"; ".join(parts)}' if parts else ', empty'))

    print('what lies where in super (sectors of 512):')
    first_data, dev_end = devices[0][0], devices[0][3] // SECTOR
    print(f'  0 .. {first_data}: liblp metadata ({size(first_data * SECTOR)})')
    at, free = first_data, 0
    for start, end, name, source in sorted(t for t in taken if t[3] == 0):
        if start > at:
            print(f'  {at} .. {start}: FREE, {size((start - at) * SECTOR)}')
            free += start - at
        print(f'  {start} .. {end}: {name}')
        at = max(at, end)
    if at < dev_end:
        print(f'  {at} .. {dev_end}: FREE, {size((dev_end - at) * SECTOR)}')
        free += dev_end - at
    print(f'{size(free * SECTOR)} of {size(devices[0][3])} in super is covered by no extent of slot {number}')
    return ok


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else 'logs/last.log'
    text = open(log_path, encoding='utf-8', errors='replace').read()
    for line in re.findall(r'^ufs: super: .*$', text, re.M):
        print(line)
    geometry = blob(text, 'geometry')
    if not geometry:
        sys.exit(f'ufs-super.py: no "ufs: super" dump in {log_path} (was the run made with ufs=1?)')
    gpt = re.search(r'^ufs: part super: lba (\d+)\.\.(\d+), (\d+) KB$', text, re.M)
    if gpt:
        print(f'GPT: super is lba {gpt[1]}..{gpt[2]}, {size(int(gpt[3]) * 1024)}')

    magic, struct_size = struct.unpack_from('<II', geometry)
    max_size, slots, block_size = struct.unpack_from('<III', geometry, 40)
    ok = checked(geometry[:struct_size], 8, 'geometry')
    print(f'  metadata up to {max_size} bytes, {slots} slots, logical block {block_size}')
    for number in range(2):
        header, tables = blob(text, f'header{number}'), blob(text, f'tables{number}')
        if header:
            ok &= slot(number, header, tables)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
