#!/usr/bin/env python3
"""Cut a partition of this port's own off the end of Android's userdata: the new GPT, made and checked here.

usage: userdata-carve.py DIR GIB [NAME]
  DIR   what was saved from the phone under Android (adb's root shell), all read only:
          gpt-head.bin             the first 8 blocks of LUN 0 (dd if=/dev/block/sda bs=4096 count=8)
          gpt-tail.bin             its last 8 blocks (... skip=BLOCKS-8 count=8)
          userdata-meta-before.bin at least the first block of userdata (its f2fs superblock)
          segment_info-before.txt  /proc/fs/f2fs/sda33/segment_info
          sb-after.bin             (once the f2fs is shrunk) the first block of userdata again
  GIB   how much to take off the end of userdata, in GiB (a whole number of f2fs segments)
  NAME  the new partition's name (default darwin)

Android's userdata on this phone is a plain f2fs (no metadata encryption: its superblock lies on the
flash as it is) that fills its partition, the last of LUN 0. Taking GIB off its end takes two steps
on the phone: first the filesystem is shrunk in place, by Android's own kernel
(tools/android/f2fs-resize, F2FS_IOC_RESIZE_FS), and only then the partition, by writing the
GPT this makes: userdata ends exactly where the shrunk f2fs ends, and a new partition NAME (of the
APFS container type) has the rest from the next 2 MB on. The kernel takes whole sections off the
main area and leaves the filesystem's own slack at its end (the blocks past the last whole segment)
as it was, so the shrunk f2fs is the old size less GIB, not the size it was asked for.

Both steps belong to one session of Android, with no reboot between them: at every boot fs_mgr
runs resize.f2fs on userdata, which grows the filesystem to whatever its partition is -- a shrunk
f2fs in the old partition is grown straight back (seen on this phone). With userdata exactly the
filesystem's size there is nothing for it to do. Nothing else in the table changes -- no other entry, no GUID, no
name, no attribute -- and both copies of the table (the primary one at the start of LUN 0, the
backup at its end) are made, each with its CRC-32s, then read back here and checked the same way a
firmware would.

What this writes, into DIR: gpt-head.new and gpt-tail.new (the same 8 blocks, with the change),
and it prints the f2fs size to shrink to and the dd commands that put the two on the phone. It
refuses unless the old table checks out, userdata is the last partition, and no f2fs segment
past the new end holds a single valid block.
"""
import re
import struct
import sys
import uuid
import zlib

BS = 4096
APFS_CONTAINER = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def header_of(block):
    if block[:8] != b'EFI PART':
        sys.exit('userdata-carve.py: no GPT header where one should be')
    size = struct.unpack_from('<I', block, 12)[0]
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    struct.pack_into('<I', h, 16, 0)
    return {
        'size': size, 'crc_right': crc(bytes(h)) == want,
        'my': struct.unpack_from('<Q', block, 24)[0], 'alternate': struct.unpack_from('<Q', block, 32)[0],
        'first_usable': struct.unpack_from('<Q', block, 40)[0], 'last_usable': struct.unpack_from('<Q', block, 48)[0],
        'entries_lba': struct.unpack_from('<Q', block, 72)[0], 'count': struct.unpack_from('<I', block, 80)[0],
        'entry_size': struct.unpack_from('<I', block, 84)[0], 'entries_crc': struct.unpack_from('<I', block, 88)[0],
    }


def seal(block, entries):
    """block with its header's entries CRC and header CRC made right for these entries"""
    b = bytearray(block)
    h = header_of(b)
    struct.pack_into('<I', b, 88, crc(entries))
    struct.pack_into('<I', b, 16, 0)
    struct.pack_into('<I', b, 16, crc(bytes(b[:h['size']])))
    return bytes(b)


def entries_of(blob, h, base):
    """the table bytes and its parsed entries, from blob that starts at LBA base"""
    at = (h['entries_lba'] - base) * BS
    raw = blob[at:at + h['count'] * h['entry_size']]
    if len(raw) != h['count'] * h['entry_size']:
        sys.exit('userdata-carve.py: the partition table is not within the saved blocks')
    parsed = []
    for i in range(h['count']):
        e = raw[i * h['entry_size']:(i + 1) * h['entry_size']]
        parsed.append({
            'type': uuid.UUID(bytes_le=e[:16]), 'guid': uuid.UUID(bytes_le=e[16:32]),
            'first': struct.unpack_from('<Q', e, 32)[0], 'last': struct.unpack_from('<Q', e, 40)[0],
            'attrs': struct.unpack_from('<Q', e, 48)[0],
            'name': e[56:128].decode('utf-16le').split('\0', 1)[0],
        })
    return raw, parsed, at


def check(name, head, tail, disk):
    """both copies: header CRCs, table CRCs, where each says the other is; the parsed table"""
    ph = header_of(head[BS:2 * BS])
    bh = header_of(tail[7 * BS:8 * BS])
    praw, pe, _ = entries_of(head, ph, 0)
    braw, be, _ = entries_of(tail, bh, disk - 8)
    ok = (ph['crc_right'] and bh['crc_right'] and crc(praw) == ph['entries_crc'] and crc(braw) == bh['entries_crc']
          and ph['my'] == 1 and ph['alternate'] == disk - 1 and bh['my'] == disk - 1 and bh['alternate'] == 1
          and praw == braw and ph['last_usable'] == bh['last_usable'] and ph['first_usable'] == bh['first_usable'])
    print(f'{name}: primary header crc {"right" if ph["crc_right"] else "WRONG"}, table crc '
          f'{"right" if crc(praw) == ph["entries_crc"] else "WRONG"}; backup header crc '
          f'{"right" if bh["crc_right"] else "WRONG"}, table crc {"right" if crc(braw) == bh["entries_crc"] else "WRONG"}; '
          f'the two tables {"the same" if praw == braw else "DIFFER"} -> {"good" if ok else "BAD"}')
    return ok, ph, bh, pe


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    d, gib = sys.argv[1], int(sys.argv[2])
    name = sys.argv[3] if len(sys.argv) > 3 else 'darwin'
    head = open(f'{d}/gpt-head.bin', 'rb').read()
    tail = open(f'{d}/gpt-tail.bin', 'rb').read()
    sb = open(f'{d}/userdata-meta-before.bin', 'rb').read(BS)[1024:2048]
    if len(head) != 8 * BS or len(tail) != 8 * BS:
        sys.exit('userdata-carve.py: gpt-head.bin and gpt-tail.bin are to be 8 blocks each')
    # the backup header says where it is: the last block of the unit
    disk = header_of(tail[7 * BS:8 * BS])['my'] + 1
    ok, ph, bh, entries = check('the table now', head, tail, disk)
    if not ok:
        sys.exit('userdata-carve.py: the table on the phone does not check out; nothing made')

    used = [i for i, e in enumerate(entries) if e['type'].int != 0]
    ud = [i for i in used if entries[i]['name'] == 'userdata']
    if len(ud) != 1:
        sys.exit('userdata-carve.py: not exactly one partition named userdata')
    u = ud[0]
    if any(entries[i]['last'] > entries[u]['last'] for i in used) or entries[u]['last'] != ph['last_usable']:
        sys.exit('userdata-carve.py: userdata is not the last partition, reaching to the end of the unit')
    if any(e['name'] == name for e in entries):
        sys.exit(f'userdata-carve.py: there is a partition named {name} already')
    free = [i for i, e in enumerate(entries) if e['type'].int == 0]
    if not free or free[0] < max(used):
        sys.exit('userdata-carve.py: no free entry after the used ones')
    slot = free[0]

    # the f2fs of userdata: where its main area is, and how its segments are filled
    magic, = struct.unpack_from('<I', sb, 0)
    log_bs, log_bps = struct.unpack_from('<I', sb, 16)[0], struct.unpack_from('<I', sb, 20)[0]
    block_count, = struct.unpack_from('<Q', sb, 36)
    segs_main, = struct.unpack_from('<I', sb, 68)
    main_blkaddr, = struct.unpack_from('<I', sb, 92)
    if magic != 0xf2f52010 or log_bs != 12:
        sys.exit('userdata-carve.py: userdata does not begin with a plain f2fs of 4 KB blocks')
    per_seg = 1 << log_bps
    part_blocks = entries[u]['last'] - entries[u]['first'] + 1
    if block_count > part_blocks:
        sys.exit('userdata-carve.py: the f2fs is bigger than its partition')
    cut = gib * 2**30 // BS // per_seg            # segments taken off the end
    new_segs = segs_main - cut
    new_blocks = main_blkaddr + new_segs * per_seg  # what F2FS_IOC_RESIZE_FS is asked for
    fs_blocks = block_count - cut * per_seg         # what the f2fs is then: its slack stays
    try:
        after = open(f'{d}/sb-after.bin', 'rb').read(BS)[1024:2048]
    except FileNotFoundError:
        after = None
    if after is not None:
        got, = struct.unpack_from('<Q', after, 36)
        got_main, = struct.unpack_from('<I', after, 68)
        print(f'f2fs on the phone now: {got} blocks, {got_main} segments in its main area')
        if (got, got_main) != (fs_blocks, new_segs):
            sys.exit(f'userdata-carve.py: expected {fs_blocks} blocks and {new_segs} segments after the shrink')
    part_end = entries[u]['first'] + fs_blocks                                        # userdata: exactly the f2fs
    new_start = (part_end + per_seg - 1) // per_seg * per_seg                          # 2 MB from the unit's start
    valid = {}
    for line in open(f'{d}/segment_info-before.txt'):
        m = re.match(r'^(\d+)\s+(.*)$', line)
        if m:
            for k, e in enumerate(m[2].split()):
                valid[int(m[1]) + k] = int(e.split('|')[1])
    if len(valid) != segs_main:
        sys.exit(f'userdata-carve.py: segment_info has {len(valid)} segments, the superblock {segs_main}')
    in_tail = [s for s in range(new_segs, segs_main) if valid[s]]
    last_used = max(s for s in valid if valid[s])
    print(f'f2fs: {block_count} blocks, main area from block {main_blkaddr}, {segs_main} segments of {per_seg} blocks; '
          f'the last segment holding data is {last_used}')
    if in_tail:
        sys.exit(f'userdata-carve.py: {len(in_tail)} segments past the new end hold data; shrink with less, '
                 f'or let the kernel move it first')

    new_first = new_start
    new_last = ph['last_usable']
    print(f'userdata: lba {entries[u]["first"]}..{entries[u]["last"]} ({part_blocks * BS / 2**30:.2f} GiB) '
          f'-> {entries[u]["first"]}..{part_end - 1} ({fs_blocks * BS / 2**30:.2f} GiB, the f2fs exactly); '
          f'{new_start - part_end} blocks between it and {name} belong to neither')
    print(f'{name}: lba {new_first}..{new_last} ({(new_last - new_first + 1) * BS / 2**30:.3f} GiB), '
          f'entry {slot} (sda{slot + 1} under Android), type APFS container')

    # the new table: userdata's end, and one new entry; every other byte as it was
    praw, _, pat = entries_of(head, ph, 0)
    table = bytearray(praw)
    es = ph['entry_size']
    struct.pack_into('<Q', table, u * es + 40, part_end - 1)
    e = bytearray(es)
    e[:16] = APFS_CONTAINER.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', e, 32, new_first, new_last, 0)
    e[56:56 + 2 * len(name)] = name.encode('utf-16le')
    table[slot * es:(slot + 1) * es] = e
    table = bytes(table)

    new_head = bytearray(head)
    new_head[pat:pat + len(table)] = table
    new_head[BS:2 * BS] = seal(head[BS:2 * BS], table)
    _, _, bat = entries_of(tail, bh, disk - 8)
    new_tail = bytearray(tail)
    new_tail[bat:bat + len(table)] = table
    new_tail[7 * BS:8 * BS] = seal(tail[7 * BS:8 * BS], table)
    new_head, new_tail = bytes(new_head), bytes(new_tail)

    ok, _, _, new_entries = check('the new table', new_head, new_tail, disk)
    changed_blocks = [i for i in range(8) if head[i * BS:(i + 1) * BS] != new_head[i * BS:(i + 1) * BS]]
    changed_tail = [i for i in range(8) if tail[i * BS:(i + 1) * BS] != new_tail[i * BS:(i + 1) * BS]]
    changed = [i for i in range(len(entries)) if entries[i] != new_entries[i]]
    print(f'blocks that differ: head {changed_blocks} (lba {changed_blocks}), tail {changed_tail} '
          f'(lba {[disk - 8 + i for i in changed_tail]}); entries that differ: {changed}')
    if not ok or changed != sorted({u, slot}):
        sys.exit('userdata-carve.py: the new table is not what it should be; nothing made')
    open(f'{d}/gpt-head.new', 'wb').write(new_head)
    open(f'{d}/gpt-tail.new', 'wb').write(new_tail)
    print(f'\nmade {d}/gpt-head.new and {d}/gpt-tail.new\n')
    print('on the phone, in this order (adb root shell):')
    print(f'  1. /data/local/tmp/f2fs-resize /data {new_blocks}     # the filesystem first')
    print(f'     and, with no reboot in between (the boot would grow it back), save its first block as sb-after.bin')
    print(f'     and run this again: it checks the shrunk size and makes the table for it')
    # each copy: its table before its header, so that a write cut short leaves a header whose table
    # CRC is wrong (and the other copy is used) rather than a right header over a half-written table
    tl, tn = ph['entries_lba'], (ph['count'] * ph['entry_size'] + BS - 1) // BS
    print(f'  2. the table: the primary copy, its table (lba {tl}..{tl + tn - 1}) and then its header (lba 1); '
          f'then the backup')
    print(f'     dd if=gpt-head.new of=/dev/block/sda bs={BS} skip={tl} seek={tl} count={tn} conv=notrunc,fsync')
    print(f'     dd if=gpt-head.new of=/dev/block/sda bs={BS} skip=1 seek=1 count=1 conv=notrunc,fsync')
    print(f'     dd if=gpt-tail.new of=/dev/block/sda bs={BS} skip={changed_tail[0]} seek={disk - 8 + changed_tail[0]} '
          f'count={changed_tail[-1] - changed_tail[0] + 1} conv=notrunc,fsync')
    return 0


if __name__ == '__main__':
    sys.exit(main())
