#!/usr/bin/env python3
"""Write an image into the phone's darwin partition, and nowhere else, from Android (adb, root).

usage:
  darwin-write.py check                 darwin's identity, read from the phone now                (writes nothing)
  darwin-write.py sum   BLOCK COUNT     sha256 of COUNT blocks of darwin from BLOCK                (writes nothing)
  darwin-write.py plan  IMAGE BLOCK     every check `write` makes, and the command it would run    (writes nothing)
  darwin-write.py write IMAGE BLOCK     with DARWIN_WRITE=yes: IMAGE at darwin's block BLOCK, read back and compared
  darwin-write.py zero  BLOCK COUNT     with DARWIN_WRITE=yes: zeros over COUNT blocks, read back and compared
  darwin-write.py selftest              the refusals, against this tool's own checks and a GPT read now (writes nothing)

Blocks are darwin's own, 4096 bytes, counted from the partition's start. What it takes to write:

  - darwin is what it was when the port was set up, read at the time of the write and compared with the
    constants below, all of them: the GPT of logical unit 0 (header and table CRCs right; entry 34 named
    darwin, its type and unique GUIDs, LBA 25374720..30617594; no other entry over any of it), sda being
    logical unit 0 of the UFS controller, /dev/block/by-name/darwin being /dev/block/sda34, whose number,
    name, start and size in sysfs and whose size by blockdev say the same. Anything else: no write.
  - the blocks lie inside darwin and after the iOS volume at its start (rootfs.apfs, blocks 0..1374207,
    which is never written): BLOCK >= 1374208 and BLOCK + COUNT <= 5242875. Anything else: no write.
  - the write goes to /dev/block/by-name/darwin only, by dd with seek and count in blocks of 4096, from a
    copy of the image on the phone whose sha256 has been checked; in the same shell, right before dd,
    the name is checked again to be sda34 of darwin's size. Never /dev/block/sda or anything else.
  - afterwards the blocks are read back and their sha256 has to be the image's.

Every attempt, refused or done, goes into logs/darwin-write.log.
"""
import binascii
import hashlib
import os
import struct
import subprocess
import sys
import time
import uuid

NAME = 'darwin'
TYPE = '7C3457EF-0000-11AA-AA11-00306543ECAC'          # Apple APFS
UNIQUE = 'FD6854AD-24AA-4FC3-94FE-290116858D68'
LUN = 0
PARTN = 34
FIRST = 25374720
LAST = 30617594
BS = 4096
BLOCKS = LAST - FIRST + 1                               # 5242875
PROTECTED = 1374208                                     # rootfs.apfs: 5628755968 bytes, never written
DEV = '/dev/block/by-name/darwin'
NODE = '/dev/block/sda34'
TMP = '/data/local/tmp/darwin-write.img'

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'logs', 'darwin-write.log')


class Refused(Exception):
    pass


def log(line):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    print('darwin-write: ' + line)


def phone(script, binary=False):
    """Run a script in the phone's root shell (on its standard input, as flash-fingerprint.sh does)."""
    r = subprocess.run(['adb', 'shell', 'su -c sh'], input=script.encode(), capture_output=True, timeout=3600)
    if r.returncode != 0:
        raise Refused('the phone said %d: %s' % (r.returncode, (r.stdout + r.stderr).decode(errors='replace')[-400:]))
    return r.stdout if binary else r.stdout.decode(errors='replace')


def phone_bytes(command):
    r = subprocess.run(['adb', 'exec-out', 'su -c "%s"' % command], capture_output=True, timeout=600)
    if r.returncode != 0:
        raise Refused('reading from the phone: exit %d' % r.returncode)
    return r.stdout


def check_gpt(raw):
    """raw: LUN 0's blocks 1.. (header, then the table). Returns the problems found."""
    bad = []
    hdr = raw[:BS]
    if hdr[:8] != b'EFI PART':
        return ['no GPT header at LBA 1']
    hsize = struct.unpack_from('<I', hdr, 12)[0]
    hcrc = struct.unpack_from('<I', hdr, 16)[0]
    h = bytearray(hdr[:hsize])
    h[16:20] = b'\0\0\0\0'
    if binascii.crc32(bytes(h)) & 0xffffffff != hcrc:
        bad.append('GPT header CRC wrong')
    last_usable = struct.unpack_from('<Q', hdr, 48)[0]
    ent_lba, n, esize = struct.unpack_from('<QII', hdr, 72)
    tcrc = struct.unpack_from('<I', hdr, 88)[0]
    if ent_lba != 2 or esize != 128 or n * esize > len(raw) - BS:
        return bad + ['GPT table not where it was (LBA %d, %d entries of %d)' % (ent_lba, n, esize)]
    table = raw[BS:BS + n * esize]
    if binascii.crc32(table) & 0xffffffff != tcrc:
        bad.append('GPT table CRC wrong')
    if last_usable < LAST:
        bad.append('last usable LBA %d is before darwin\'s end' % last_usable)
    found = []
    for i in range(n):
        e = table[i * esize:(i + 1) * esize]
        if e[:16] == b'\0' * 16:
            continue
        name = e[56:128].decode('utf-16le', errors='replace').rstrip('\0')
        first, last = struct.unpack_from('<QQ', e, 32)
        if name == NAME:
            found.append((i + 1, str(uuid.UUID(bytes_le=e[:16])).upper(), str(uuid.UUID(bytes_le=e[16:32])).upper(),
                          first, last))
        elif first <= LAST and last >= FIRST:
            bad.append('entry %d (%s, LBA %d..%d) overlaps darwin' % (i + 1, name, first, last))
    if len(found) != 1:
        return bad + ['%d entries named darwin' % len(found)]
    num, typ, uniq, first, last = found[0]
    for what, got, want in (('number', num, PARTN), ('type', typ, TYPE), ('unique GUID', uniq, UNIQUE),
                            ('first LBA', first, FIRST), ('last LBA', last, LAST)):
        if got != want:
            bad.append('darwin\'s %s is %s, not %s' % (what, got, want))
    return bad


def identity():
    """darwin's identity, read now. Raises Refused on anything that is not as it was."""
    out = phone('''
echo "node=$(readlink -f %(dev)s)"
echo "lun=$(readlink -f /sys/class/block/sda/device)"
echo "partition=$(cat /sys/class/block/sda%(n)d/partition)"
echo "start=$(cat /sys/class/block/sda%(n)d/start)"
echo "size=$(cat /sys/class/block/sda%(n)d/size)"
echo "name=$(grep ^PARTNAME= /sys/class/block/sda%(n)d/uevent | cut -d= -f2)"
echo "bytes=$(blockdev --getsize64 %(dev)s)"
echo "ss=$(blockdev --getss /dev/block/sda)"
''' % {'dev': DEV, 'n': PARTN})
    facts = dict(line.split('=', 1) for line in out.split('\n') if '=' in line)
    bad = []
    want = {'node': NODE, 'partition': str(PARTN), 'start': str(FIRST * BS // 512), 'size': str(BLOCKS * BS // 512),
            'name': NAME, 'bytes': str(BLOCKS * BS), 'ss': str(BS)}
    for k, v in want.items():
        if facts.get(k) != v:
            bad.append('%s is %r, not %r' % (k, facts.get(k), v))
    lun = facts.get('lun', '')
    if 'ufshc' not in lun or not lun.endswith(':%d' % LUN):
        bad.append('sda is %r, not logical unit %d of the UFS controller' % (lun, LUN))
    # the GPT: LBA 1 (header) and the table after it (64 entries of 128 bytes: LBA 2..3)
    raw = phone_bytes('dd if=/dev/block/sda bs=%d skip=1 count=3 2>/dev/null' % BS)
    if len(raw) != 3 * BS:
        bad.append('read %d bytes of the GPT, not %d' % (len(raw), 3 * BS))
    else:
        bad += check_gpt(raw)
    if bad:
        raise Refused('darwin is not what it was: ' + '; '.join(bad))
    return 'darwin is %s, entry %d of logical unit %d, LBA %d..%d (%d blocks of %d), %s, %s' % (
        NODE, PARTN, LUN, FIRST, LAST, BLOCKS, BS, TYPE, UNIQUE)


def blocks_ok(block, count):
    """The range rule; raises Refused."""
    if not (isinstance(block, int) and isinstance(count, int)):
        raise Refused('blocks must be whole numbers')
    if count <= 0:
        raise Refused('nothing to write (%d blocks)' % count)
    if block < PROTECTED:
        raise Refused('block %d is inside the iOS volume (blocks 0..%d), which is never written' % (block, PROTECTED - 1))
    if block + count > BLOCKS:
        raise Refused('blocks %d..%d go past darwin\'s end (%d blocks)' % (block, block + count - 1, BLOCKS))


def number(s):
    if not s.isdigit():
        raise Refused('%r is not a block number' % s)
    return int(s)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                return h.hexdigest()
            h.update(b)


def sha256_zeros(n):
    h = hashlib.sha256()
    z = b'\0' * (1 << 20)
    while n > 0:
        h.update(z[:min(n, len(z))])
        n -= len(z)
    return h.hexdigest()


def phone_sum(block, count):
    blocks_ok_read(block, count)
    out = phone('dd if=%s bs=%d skip=%d count=%d 2>/dev/null | sha256sum\n' % (DEV, BS, block, count))
    return out.split()[0]


def blocks_ok_read(block, count):
    if count <= 0 or block < 0 or block + count > BLOCKS:
        raise Refused('blocks %d..%d are not inside darwin' % (block, block + count - 1))


def guarded(command):
    """command, run only if the name still leads to darwin, checked in the same shell right before it."""
    return ('t=$(readlink -f %(dev)s); s=$(blockdev --getsize64 %(dev)s)\n'
            'if [ "$t" = %(node)s ] && [ "$s" = %(bytes)d ]; then %(cmd)s 2>&1; else echo "REFUSED: $t $s"; exit 3; fi\n'
            % {'dev': DEV, 'node': NODE, 'bytes': BLOCKS * BS, 'cmd': command})


def prepare(image, block):
    size = os.path.getsize(image)
    if size % BS:
        raise Refused('%s is %d bytes, not whole blocks of %d' % (image, size, BS))
    count = size // BS
    blocks_ok(block, count)
    return count


def cmd_plan(image, block, write=False):
    count = prepare(image, block)
    who = identity()
    digest = sha256_file(image)
    dd = 'dd if=%s of=%s bs=%d seek=%d count=%d conv=notrunc,fsync' % (TMP, DEV, BS, block, count)
    log('%s: %s (%d blocks, sha256 %s) at darwin blocks %d..%d (bytes %d..%d); %s; would run: %s' % (
        'write' if write else 'plan', image, count, digest, block, block + count - 1, block * BS,
        (block + count) * BS - 1, who, dd))
    if not write:
        return
    if os.environ.get('DARWIN_WRITE') != 'yes':
        raise Refused('DARWIN_WRITE=yes is not set: nothing written')
    r = subprocess.run(['adb', 'push', image, TMP], capture_output=True, text=True)
    if r.returncode != 0:
        raise Refused('adb push: ' + r.stderr.strip())
    try:
        there = phone('sha256sum %s\n' % TMP).split()[0]
        if there != digest:
            raise Refused('the copy on the phone has sha256 %s, not %s' % (there, digest))
        identity()                                   # once more, right before the write
        out = phone(guarded(dd))
        log('dd: ' + ' '.join(out.split()))
        back = phone_sum(block, count)
        log('read back blocks %d..%d: sha256 %s -- %s' % (block, block + count - 1, back,
                                                          'the same' if back == digest else 'DIFFERENT'))
        if back != digest:
            raise Refused('what was read back is not what was written')
    finally:
        phone('rm -f %s\n' % TMP)


def cmd_zero(block, count):
    blocks_ok(block, count)
    who = identity()
    dd = 'dd if=/dev/zero of=%s bs=%d seek=%d count=%d conv=notrunc,fsync' % (DEV, BS, block, count)
    log('zero: darwin blocks %d..%d; %s; runs: %s' % (block, block + count - 1, who, dd))
    if os.environ.get('DARWIN_WRITE') != 'yes':
        raise Refused('DARWIN_WRITE=yes is not set: nothing written')
    out = phone(guarded(dd))
    log('dd: ' + ' '.join(out.split()))
    back, want = phone_sum(block, count), sha256_zeros(count * BS)
    log('read back blocks %d..%d: %s' % (block, block + count - 1, 'zeros' if back == want else 'NOT ZEROS ' + back))
    if back != want:
        raise Refused('what was read back is not zeros')


def cmd_selftest():
    """The refusals: each must refuse, and none of them reaches the phone's dd."""
    ok = True
    for block, count, why in ((PROTECTED - 1, 256, 'one block into the iOS volume'), (0, 1, 'the iOS volume\'s superblock'),
                              (BLOCKS - 100, 256, 'past darwin\'s end'), (BLOCKS, 1, 'right after darwin'),
                              (-1, 1, 'before darwin'), (PROTECTED, 0, 'nothing'), (10 ** 12, 1, 'far away')):
        try:
            blocks_ok(block, count)
            log('selftest: %s (blocks %d+%d) was NOT refused' % (why, block, count))
            ok = False
        except Refused as e:
            log('selftest: %s refused: %s' % (why, e))
    for s in ('-5', '1e6', '0x100', ''):
        try:
            number(s)
            ok = False
            log('selftest: block %r was NOT refused' % s)
        except Refused as e:
            log('selftest: block %r refused: %s' % (s, e))
    raw = phone_bytes('dd if=/dev/block/sda bs=%d skip=1 count=3 2>/dev/null' % BS)
    log('selftest: the GPT read now: %s' % ('; '.join(check_gpt(raw)) or 'darwin as it was'))
    ok &= not check_gpt(raw)
    # the same GPT changed in one place at a time: each must be refused
    def entry_off(field):
        return BS + (PARTN - 1) * 128 + field
    for what, off, value in (('a type GUID byte', entry_off(0), None), ('a unique GUID byte', entry_off(20), None),
                             ('the first LBA', entry_off(32), struct.pack('<Q', FIRST - 8)),
                             ('the last LBA', entry_off(40), struct.pack('<Q', LAST + 1)),
                             ('the name', entry_off(56), 'Darwin'.encode('utf-16le'))):
        bad = bytearray(raw)
        if value is None:
            bad[off] ^= 0x01
        else:
            bad[off:off + len(value)] = value
        # with the table's CRC made right again, so that the field itself is what is caught
        n = struct.unpack_from('<I', bad, 80)[0]
        struct.pack_into('<I', bad, 88, binascii.crc32(bytes(bad[BS:BS + n * 128])) & 0xffffffff)
        h = bytearray(bad[:struct.unpack_from('<I', bad, 12)[0]])
        h[16:20] = b'\0\0\0\0'
        struct.pack_into('<I', bad, 16, binascii.crc32(bytes(h)) & 0xffffffff)
        problems = check_gpt(bytes(bad))
        log('selftest: GPT with %s changed: %s' % (what, '; '.join(problems) if problems else 'NOT refused'))
        ok &= bool(problems)
    bad = bytearray(raw)
    bad[BS + (PARTN - 1) * 128 + 40] ^= 0x01                 # without fixing the CRC: the CRC catches it
    problems = check_gpt(bytes(bad))
    log('selftest: GPT with a byte changed and the CRC left: %s' % ('; '.join(problems) or 'NOT refused'))
    ok &= bool(problems)
    log('selftest: %s' % ('every refusal refused, the GPT as it was' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    try:
        if a == ['check']:
            log('check: ' + identity())
        elif len(a) == 3 and a[0] == 'sum':
            block, count = number(a[1]), number(a[2])
            identity()
            log('sum: darwin blocks %d..%d: sha256 %s (zeros would be %s)' % (block, block + count - 1,
                                                                            phone_sum(block, count),
                                                                            sha256_zeros(count * BS)))
        elif len(a) == 3 and a[0] in ('plan', 'write'):
            cmd_plan(a[1], number(a[2]), write=(a[0] == 'write'))
        elif len(a) == 3 and a[0] == 'zero':
            cmd_zero(number(a[1]), number(a[2]))
        elif a == ['selftest']:
            sys.exit(0 if cmd_selftest() else 1)
        else:
            sys.exit(__doc__)
    except Refused as e:
        log('REFUSED %s: %s' % (' '.join(a), e))
        sys.exit(2)


if __name__ == '__main__':
    main()
