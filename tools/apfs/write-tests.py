#!/usr/bin/env python3
"""Judge apfs_write.c on images on this machine, where a ruined one costs nothing.

Driven by tools/apfs/write-tests.sh, which builds what this needs.  Nothing here reads the
image with the code under test alone: every claim about what is in an image afterwards is made
by at least one reader that is not apfs_write.c's and preferably by all of them --

  apfsck      linux-apfs/apfsprogs: checksums, every tree, the space manager's bitmaps against
              every block anything refers to, reference counts, extents.  Knows nothing of us.
  apfs.py     this repository's own Python reader, which verify.py has already checked against
              7-Zip over the whole iOS root volume.
  7-Zip       a third implementation entirely.
  apfs-tool   the kernel's own apfs_lib.c -- the one reader that is partly the code under test
              (it shares struct definitions), so it is never the only witness.

and by the one witness that needs no reader at all: which blocks of the image changed.  A write
in place must change blocks that hold the contents of the one file named and not one block more,
and that is checked by comparing the image with the copy it was made from, block by block.

usage: write-tests.py [-k] [TEST ...]       (-k: keep the images in the work directory)
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import apfs                                                     # noqa: E402  (after sys.path)
import mkapfs                                                   # noqa: E402

BS = 4096
WORK = os.path.join(ROOT, 'apfs-work', 'write-tests')
TOOL = os.path.join(ROOT, 'apfs-work', 'apfs-tool')
APFSCK = os.path.join(ROOT, 'xnu-deps', 'build', 'apfsprogs', 'apfsck', 'apfsck')

failures = []
done = 0


def say(ok, what):
    global done
    done += 1
    print('%s %s' % ('[ ok ]' if ok else '[FAIL]', what))
    if not ok:
        failures.append(what)
    return ok


def run(*argv, **kw):
    """A command, with its output; never raises -- the caller judges the exit status."""
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


# ---------------------------------------------------------------- the image under test

#: the tree every test image is made of: the sizes a write has to get right
TREE = {
    'empty': 0,
    'one': 1,
    'small': BS - 1,
    'block': BS,
    'block1': BS + 1,
    'two': 2 * BS,
    'big': 1 << 20,
    'sub/text': 1300,
}


def make_tree(at):
    """Random bytes, the same every run: a file that compresses well would be stored
    compressed, which is a case of its own (test_compressed)."""
    import random

    rng = random.Random(20261019)
    os.makedirs(os.path.join(at, 'sub'), exist_ok=True)
    for name, size in sorted(TREE.items()):
        with open(os.path.join(at, name), 'wb') as f:
            f.write(bytes(rng.randrange(256) for _ in range(size)))


def make_image(path, compress=False):
    tree = os.path.join(WORK, 'tree')
    make_tree(tree)
    argv = [sys.executable, os.path.join(HERE, 'mkapfs.py'), '-o', path]
    if compress:
        argv.append('-z')
    argv.append('data=' + tree)
    rc, out = run(*argv)
    if rc != 0:
        raise SystemExit('write-tests.py: mkapfs.py failed:\n' + out)
    return path


# ---------------------------------------------------------------- the readers

def read_apfsck(image, crash=True):
    """-w: is the container sound.  -c as well: and does it also look cleanly unmounted, which
    is a different question -- a container committed and not yet unmounted is sound but not
    clean, because block zero still holds the superblock copy from before."""
    rc, out = run(APFSCK, '-cw' if crash else '-w', image)
    return rc == 0, out


def read_py(image, path):
    rc, out = run(sys.executable, os.path.join(HERE, 'apfs.py'), 'cat', image, path)
    return None if rc != 0 else out


def read_py_bytes(image, path):
    p = subprocess.run([sys.executable, os.path.join(HERE, 'apfs.py'), 'cat', image, path],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return None if p.returncode != 0 else p.stdout


def read_tool_bytes(image, path):
    p = subprocess.run([TOOL, 'cat', image, path], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return None if p.returncode != 0 else p.stdout


def read_7z_bytes(image, path):
    p = subprocess.run(['7z', 'x', '-so', image, path.lstrip('/')], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return None if p.returncode != 0 else p.stdout


READERS = [('apfs.py', read_py_bytes), ('7-Zip', read_7z_bytes), ('apfs-tool', read_tool_bytes)]


def contents(image, path):
    """What every reader says the file holds, as a dict reader -> bytes (or None)."""
    return dict((name, fn(image, path)) for name, fn in READERS)


def all_agree(image, path, want):
    got = contents(image, path)
    bad = [n for n, b in got.items() if b != want]
    return not bad, ', '.join('%s: %s' % (n, 'cannot read' if got[n] is None else
                                          'sha %s' % hashlib.sha256(got[n]).hexdigest()[:12])
                              for n in bad)


def census(image):
    """Every entry of the volume with a hash of its contents, by the Python reader: the
    witness that no other file was touched."""
    rc, out = run(sys.executable, os.path.join(HERE, 'verify.py'), 'clines', image)
    return out if rc == 0 else None


# ---------------------------------------------------------------- which blocks changed

def changed_blocks(a, b):
    """The blocks where two images differ.  No reader, no structure: just bytes."""
    out = []
    fa, fb = open(a, 'rb'), open(b, 'rb')
    n = 0
    while True:
        x, y = fa.read(BS), fb.read(BS)
        if not x and not y:
            break
        if x != y:
            out.append(n)
        n += 1
    fa.close()
    fb.close()
    return out


def extents_of(image, path):
    """Where the file's bytes are, by the Python reader: [(logical, length, phys), ...]."""
    c = apfs.Container(image)
    try:
        vol = c.volumes()[0]
        oid, _dtype = vol.lookup(path)
        return vol.extents(vol.inode(oid).private_id)
    finally:
        c.close()


def stream_of(image, path):
    c = apfs.Container(image)
    try:
        vol = c.volumes()[0]
        oid, _dtype = vol.lookup(path)
        return vol.inode(oid).private_id
    finally:
        c.close()


def blocks_of(image, path):
    s = set()
    for _logical, length, phys in extents_of(image, path):
        if phys:
            s.update(range(phys, phys + length // BS))
    return s


# ---------------------------------------------------------------- poking an image

def reseal(image, bno):
    """Put the Fletcher-64 of a block back after changing it, the way mkapfs.py seals one."""
    with open(image, 'r+b') as f:
        f.seek(bno * BS)
        blk = bytearray(f.read(BS))
        struct.pack_into('<Q', blk, 0, mkapfs.fletcher64(bytes(blk)))
        f.seek(bno * BS)
        f.write(bytes(blk))


def find_object(image, want_type, want_subtype=None, leaf_only=False):
    """Every block of the image that carries an object of this type: a scan, which is fine
    because the test images are a few megabytes."""
    found = []
    size = os.path.getsize(image)
    with open(image, 'rb') as f:
        for bno in range(size // BS):
            f.seek(bno * BS)
            blk = f.read(BS)
            if len(blk) < 32 or blk[0:8] == b'\0' * 8:
                continue
            otype = struct.unpack_from('<I', blk, 24)[0]
            subtype = struct.unpack_from('<I', blk, 28)[0]
            if (otype & 0xffff) != want_type:
                continue
            if want_subtype is not None and (subtype & 0xffff) != want_subtype:
                continue
            if leaf_only and not (struct.unpack_from('<H', blk, 32)[0] & 0x0002):
                continue
            found.append(bno)
    return found


def patch(image, bno, off, data, seal=True):
    with open(image, 'r+b') as f:
        f.seek(bno * BS + off)
        f.write(data)
    if seal:
        reseal(image, bno)


def find_record(image, bno, key):
    """Where the value of the record with this key lives in this block, read with apfs.py's own
    node parser so that a poke cannot land in the wrong place: (offset in the block, length)."""
    with open(image, 'rb') as f:
        f.seek(bno * BS)
        blk = f.read(BS)
    n = apfs.Node(bno, blk, BS)
    if not n.is_leaf or n.fixed:
        return None, None
    for i in range(n.nkeys):
        koff, klen, voff, vlen = struct.unpack_from('<HHHH', blk, n.toc_off + 8 * i)
        k = blk[n.key_base + koff:n.key_base + koff + klen]
        if k == key and voff != 0xFFFF:
            return n.val_end - voff, vlen
    return None, None



# ---------------------------------------------------------------- the store, read independently

#: a second reading of the store's format, from bsd/miscfs/apfs/apfs_store.h and nothing else, so
#: that what the C writes is never checked only by the C that wrote it
STORE_MAGIC = b'RDWSTOR\x00'
STORE_PAYLOAD = 4096
FNV_INIT = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


def fnv1a(data, h=FNV_INIT):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def store_slot_bytes(image, slot):
    """A whole slot file, read by apfs.py."""
    return read_py_bytes(image, '/slot%d' % slot)


def store_head_py(image, slot, payload_max):
    """The header of a slot, or None when it holds nothing good. Checks what apfs_store.c checks:
    the magic, its own sum, the version, that the slot says which slot it is, and the length."""
    blk = store_slot_bytes(image, slot)
    if blk is None or len(blk) < STORE_PAYLOAD or blk[:8] != STORE_MAGIC:
        return None
    (version, which) = struct.unpack_from('<II', blk, 8)
    (generation, nbytes, seconds, note, summ, head_sum) = struct.unpack_from('<QQQQQQ', blk, 16)
    if head_sum != fnv1a(blk[:56]):
        return None
    if version != 1 or which != slot or generation == 0 or nbytes > payload_max:
        return None
    return dict(slot=slot, generation=generation, bytes=nbytes, seconds=seconds, note=note,
                sum=summ)


def store_newest_py(image, payload_max):
    """The newest good slot and its payload, by this reader alone: the header sums right and the
    payload sums to what the header says."""
    best = None
    for slot in (0, 1):
        h = store_head_py(image, slot, payload_max)
        if h is None:
            continue
        whole = store_slot_bytes(image, slot)
        payload = whole[STORE_PAYLOAD:STORE_PAYLOAD + h['bytes']]
        if len(payload) != h['bytes'] or fnv1a(payload) != h['sum']:
            continue
        if best is None or h['generation'] > best[0]['generation']:
            best = (h, payload)
    return best


def store_image(name, slot_bytes='64K', slots=2):
    out = os.path.join(WORK, name + '.apfs')
    rc, out_text = run(sys.executable, os.path.join(HERE, 'mkstore.py'), '-o', out,
                       '--slot-bytes', slot_bytes, '--slots', str(slots))
    if rc != 0:
        raise SystemExit('write-tests.py: mkstore.py failed:\n' + out_text)
    return out


def store_payload_max(image):
    c = apfs.Container(image)
    try:
        vol = c.volumes()[0]
        oid, _t = vol.lookup('/slot0')
        return vol.inode(oid).size - STORE_PAYLOAD
    finally:
        c.close()


def store_save(image, data, note=0, fail_after=None):
    argv = [TOOL, 'store', 'save', image, '-', str(note)]
    if fail_after is not None:
        argv += ['--fail-after', str(fail_after)]
    p = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def store_load(image):
    p = subprocess.run([TOOL, 'store', 'load', image, '-'], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return None if p.returncode != 0 else p.stdout


def store_blocks(image):
    return blocks_of(image, '/slot0') | blocks_of(image, '/slot1')


def test_store():
    """The store of fixed-length slot files: saves alternate between the slots, a load takes the
    newest, and nothing outside the slot files is ever written."""
    img = store_image('store')
    payload_max = store_payload_max(img)
    pristine = os.path.join(WORK, 'store-pristine.apfs')
    shutil.copyfile(img, pristine)
    ok, ck = read_apfsck(img)
    say(ok, 'the fresh store volume is clean to apfsck' + ('' if ok else ':\n' + ck))
    say(store_newest_py(img, payload_max) is None and store_load(img) is None,
        'a fresh store holds nothing good, by either reader')

    slots_seen, good = [], True
    want = None
    for i in range(5):
        data = bytes(((i + 3) * 37 + j) & 0xff for j in range(1000 + i * 4000))
        rc, out = store_save(img, data, note=100 + i)
        if rc != 0:
            good = say(False, '  save %d failed:\n%s' % (i, out))
            break
        slots_seen.append(int(out.split('into slot ')[1].split()[0]))
        want = data
        # the C's own load, the independent Python reader, and apfsck, after every save
        got = store_load(img)
        py = store_newest_py(img, payload_max)
        ck_ok, ck = read_apfsck(img)
        if got != want:
            good = say(False, '  save %d: the C reads back something else' % i)
            break
        if py is None or py[1] != want or py[0]['generation'] != i + 1 or py[0]['note'] != 100 + i:
            good = say(False, '  save %d: the Python reader disagrees (%s)'
                       % (i, 'nothing good' if py is None else py[0]))
            break
        if not ck_ok:
            good = say(False, '  save %d: apfsck is unhappy:\n%s' % (i, ck))
            break
    say(good, 'five saves in turn: both readers agree every time and apfsck stays clean')
    say(slots_seen == [0, 1, 0, 1, 0],
        'and they alternate between the slots: %s' % slots_seen)
    diff = set(changed_blocks(pristine, img))
    target = store_blocks(pristine)
    say(diff <= target, 'of the whole volume only the slot files changed (%d blocks of their %d)'
        % (len(diff), len(target)) if diff <= target else
        'blocks outside the slots changed: %s' % sorted(diff - target)[:8])


def test_store_refusals():
    """What the store refuses, and each is a way of being pointed at the wrong thing."""
    # a volume that is not the store: the name is the gate
    base, img = fresh('store-notstore')
    rc, out = store_save(img, b'x' * 100)
    say(rc != 0 and 'not named' in out, 'refused: a volume that is not named RedwoodStore' +
        ('' if rc != 0 else ':\n' + out.strip()))
    say(changed_blocks(base, img) == [], '  and nothing in it was written')

    # a payload longer than a slot will hold
    img = store_image('store-full', slot_bytes='8K')
    payload_max = store_payload_max(img)
    rc, out = store_save(img, b'y' * (payload_max + 1))
    say(rc != 0 and 'will hold' in out, 'refused: a payload longer than the slot' +
        ('' if rc != 0 else ':\n' + out.strip()))
    say(store_newest_py(img, payload_max) is None,
        '  and the slot it was going into holds nothing good afterwards')

    # exactly as much as it will hold goes in
    rc, out = store_save(img, b'z' * payload_max)
    py = store_newest_py(img, payload_max)
    say(rc == 0 and py is not None and py[1] == b'z' * payload_max,
        'a payload of exactly the slot\'s length goes in and reads back')

    # a volume shaped wrongly: slots of different lengths, a slot missing, a slot compressed.
    # Built as such rather than poked into being such, so the refusal is for the stated reason.
    for what, sizes, compress, expect in (
            ('slots that are not the same length', {'slot0': 65536, 'slot1': 32768}, False,
             'not the same length'),
            ('a store with only one slot', {'slot0': 65536}, False, 'no such slot file'),
            ('a slot too short to hold a header', {'slot0': 4096, 'slot1': 4096}, False,
             'too short'),
            ('a slot the volume stored compressed', {'slot0': 65536, 'slot1': 65536}, True,
             'compressed')):
        tree = os.path.join(WORK, 'oddtree')
        shutil.rmtree(tree, ignore_errors=True)
        os.makedirs(tree)
        for name, size in sizes.items():
            with open(os.path.join(tree, name), 'wb') as f:
                f.truncate(size)        # all zeros, so -z stores it compressed
        img = os.path.join(WORK, 'odd.apfs')
        argv = [sys.executable, os.path.join(HERE, 'mkapfs.py'), '-o', img]
        if compress:
            argv.append('-z')
        argv.append('RedwoodStore=' + tree)
        rc, out = run(*argv)
        if rc != 0:
            say(False, '%s: the image could not be made:\n%s' % (what, out))
            continue
        before = open(img, 'rb').read()
        rc, out = store_save(img, b'q' * 100)
        unchanged = open(img, 'rb').read() == before
        say(rc != 0 and expect in out and unchanged, 'refused: %s' % what +
            ('' if rc != 0 else ': it was NOT refused') +
            ('' if expect in out else ': for the wrong reason -- %s' % out.strip()) +
            ('' if unchanged else ' -- and the image changed'))

def test_store_power_failure():
    """The power out at every block of a save.  A save is: the slot's header zeroed, the payload,
    then the header -- so at every moment either the slot being written says nothing good (and the
    other slot's generation is what a reader takes), or the save has finished.  Never a mixture."""
    img = store_image('store-pf', slot_bytes='64K')
    payload_max = store_payload_max(img)
    first = bytes((j * 11 + 5) & 0xff for j in range(20000))
    rc, out = store_save(img, first, note=1)
    if not say(rc == 0, 'a first save to fail over' + ('' if rc == 0 else ':\n' + out)):
        return
    base = os.path.join(WORK, 'store-pf-base.apfs')
    shutil.copyfile(img, base)
    second = bytes((j * 7 + 200) & 0xff for j in range(40000))
    rc, out = store_save(img, second, note=2)
    total = int(out.split('; ')[1].split()[0])
    good = True
    for k in range(total + 1):
        part = os.path.join(WORK, 'store-pf-%d.apfs' % k)
        shutil.copyfile(base, part)
        rc, out = store_save(part, second, note=2, fail_after=k)
        ck_ok, ck = read_apfsck(part)
        if not ck_ok:
            good = say(False, '  stopped after %d blocks: apfsck is unhappy:\n%s' % (k, ck))
            continue
        diff = set(changed_blocks(base, part))
        if not diff <= store_blocks(base):
            good = say(False, '  stopped after %d blocks: blocks outside the slots changed: %s'
                       % (k, sorted(diff - store_blocks(base))[:6]))
            continue
        # what both readers see now must be one of exactly two things
        py = store_newest_py(part, payload_max)
        c_bytes = store_load(part)
        if py is None:
            good = say(False, '  stopped after %d blocks: no slot holds anything good' % k)
            continue
        if py[1] != c_bytes:
            good = say(False, '  stopped after %d blocks: the two readers disagree' % k)
            continue
        if rc == 0:
            if py[1] != second or py[0]['generation'] != 2:
                good = say(False, '  stopped after %d blocks: it reported success but the new '
                           'payload is not what reads back' % k)
            continue
        if py[1] != first or py[0]['generation'] != 1 or py[0]['note'] != 1:
            good = say(False, '  stopped after %d blocks: the save did not finish, yet what '
                       'reads back is neither the old payload nor the new' % k)
            continue
    say(good, 'the power out after each of the %d blocks of a save: the volume is sound every '
        'time, and what reads back is either the save before it or this one, never a mixture'
        % total)

    # and the cold case: the very first save into an empty store, stopped partway.  There is no
    # slot before it to fall back on, so a reader must say plainly that there is nothing, which
    # is what init needs to hear to leave /private/var as it made it.
    cold = store_image('store-cold', slot_bytes='64K')
    cold_base = os.path.join(WORK, 'store-cold-base.apfs')
    shutil.copyfile(cold, cold_base)
    good = True
    for k in range(total):
        part = os.path.join(WORK, 'store-cold-%d.apfs' % k)
        shutil.copyfile(cold_base, part)
        rc, out = store_save(part, second, note=9, fail_after=k)
        ck_ok, ck = read_apfsck(part)
        if not ck_ok:
            good = say(False, '  cold, stopped after %d blocks: apfsck is unhappy:\n%s' % (k, ck))
            continue
        if rc == 0:
            continue                    # it finished; the full sweep above covers that
        if store_newest_py(part, payload_max) is not None or store_load(part) is not None:
            good = say(False, '  cold, stopped after %d blocks: a reader thinks there is '
                       'something good in there' % k)
    say(good, 'the first save into an empty store, stopped at each of its blocks: both readers '
        'say there is nothing rather than something half written')


# ---------------------------------------------------------------- the tests

def write(image, path, at, data, fail_after=None, corrupt_after=None):
    """apfs-tool write: the kernel's apfs_write.c on this image."""
    argv = [TOOL, 'write', image, path, str(at), '-']
    if fail_after is not None:
        argv += ['--fail-after', str(fail_after)]
    if corrupt_after is not None:
        argv += ['--corrupt-after', str(corrupt_after)]
    p = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def fresh(name, compress=False):
    """A pristine image and a copy of it to write on, so the two can be compared."""
    base = os.path.join(WORK, 'base%s.apfs' % ('-z' if compress else ''))
    if not os.path.exists(base):
        make_image(base, compress)
    out = os.path.join(WORK, name + '.apfs')
    shutil.copyfile(base, out)
    return base, out


def expect_contents(path, at, data):
    """What the file should hold after the write: its old bytes with `data` dropped in."""
    base = os.path.join(WORK, 'base.apfs')
    old = read_py_bytes(base, path)
    return old[:at] + data + old[at + len(data):]


def test_baseline():
    """The image we start from is sound and all four readers agree about it."""
    base, _ = fresh('baseline')
    ok, out = read_apfsck(base)
    say(ok, 'the fresh image is clean to apfsck' + ('' if ok else ':\n' + out))
    good = True
    for path in sorted(TREE):
        want = read_py_bytes(base, '/' + path)
        agree, bad = all_agree(base, '/' + path, want)
        good = good and agree and want is not None and len(want) == TREE[path]
        if not agree:
            print('       /%s: %s' % (path, bad))
    say(good, 'every reader reads every file of the fresh image the same')


def do_write(name, path, at, data, why=None):
    """One write that is expected to go through, judged every way there is."""
    base, img = fresh(name)
    before = census(base)
    target = blocks_of(base, path)
    rc, out = write(img, path, at, data)
    if not say(rc == 0, 'write %d bytes at %d of %s' % (len(data), at, path) +
               ('' if rc == 0 else ':\n' + out.strip())):
        return
    want = expect_contents(path, at, data)
    agree, bad = all_agree(img, path, want)
    say(agree, '  every reader reads back what was written' + ('' if agree else ': ' + bad))
    ok, ck = read_apfsck(img)
    say(ok, '  apfsck still finds the container clean' + ('' if ok else ':\n' + ck))
    diff = set(changed_blocks(base, img))
    say(diff <= target, '  only the file\'s own blocks changed (%d of them, all inside its %d)'
        % (len(diff), len(target)) if diff <= target else
        '  blocks outside the file changed: %s' % sorted(diff - target))
    after = census(img)
    moved = [l for l in (after or '').splitlines() if l not in (before or '').splitlines()]
    say(len(moved) == 1 and path in moved[0],
        '  of the whole volume only %s reads differently' % path if len(moved) == 1 else
        '  these entries changed, not just the one: %s' % moved[:4])


def test_writes():
    do_write('w-aligned', '/block', 0, b'\xa5' * BS)
    do_write('w-unaligned', '/block1', 4090, b'Z' * 7)       # across the block boundary, to EOF
    do_write('w-middle', '/big', 5000, bytes(range(256)) * 40)
    do_write('w-tail', '/small', BS - 2, b'\x01\x02'[:1])
    do_write('w-one', '/one', 0, b'!')
    do_write('w-whole', '/two', 0, b'\x5a' * (2 * BS))
    do_write('w-cross', '/two', BS - 3, b'\xee' * 6)


def expect_refusal(name, path, at, data, what, compress=False):
    base, img = fresh(name, compress)
    rc, out = write(img, path, at, data)
    refused = rc != 0
    unchanged = changed_blocks(base, img) == []
    say(refused and unchanged, 'refused: %s' % what +
        ('' if refused and unchanged else
         (' -- it was NOT refused' if not refused else '') +
         (' -- and the image changed' if not unchanged else '')))
    return out


def test_refusals():
    expect_refusal('r-past', '/block', BS, b'x', 'a write that starts at the end of the file')
    expect_refusal('r-over', '/block', BS - 1, b'xx', 'a write that would run past the end')
    expect_refusal('r-empty', '/empty', 0, b'x', 'any write into a file of no length')
    expect_refusal('r-dir', '/sub', 0, b'x', 'a write into a directory')
    expect_refusal('r-zcomp', '/sub/text', 0, b'x', 'a write into a compressed file',
                   compress=True)


def test_volume_gates():
    """The checks that are made of the volume before any file is named.  Each is tested by
    changing the volume superblock, which apfsck would rightly dislike -- the point is only
    that the writer refuses."""
    for what, off, value in (('a sealed volume', 56, 0x08 | 0x20),
                             ('a volume with snapshots', 216, 1),
                             ('a volume with integrity metadata', 1024, 999),
                             ("iOS's own system volume, by its role", 964, 1),
                             ('the preboot volume, by its role', 964, 0x10)):
        base, img = fresh('g-%d-%d' % (off, value))
        sb = find_object(img, 0x0d)             # APFS_OBJECT_TYPE_FS: the volume superblock
        if len(sb) != 1:
            say(False, '%s: expected one volume superblock, found %d' % (what, len(sb)))
            continue
        patch(img, sb[0], off, struct.pack('<Q', value))
        rc, out = write(img, '/block', 0, b'q' * BS)
        say(rc != 0, 'refused: %s' % what + ('' if rc != 0 else ':\n' + out.strip()))


def test_extent_gates():
    """The checks that are made of the blocks themselves.  Each one is set up by changing a
    record of a tree so that it says something a correct volume never says, and each one
    stands between a misread tree and somebody else's data."""
    fresh('e-base')                             # so that base.apfs is there to read
    stream = stream_of(os.path.join(WORK, 'base.apfs'), '/block')

    # a hole: the file's own extent record saying there is no block here
    base, img = fresh('e-hole')
    leaves = find_object(img, 0x03, 0x0e, leaf_only=True) + find_object(img, 0x02, 0x0e)
    did = False
    for bno in leaves:
        key = struct.pack('<Q', (8 << 60) | stream) + struct.pack('<Q', 0)
        v_at, vl = find_record(img, bno, key)
        if v_at is not None and vl >= 24:
            patch(img, bno, v_at + 8, struct.pack('<Q', 0))     # phys_block_num = 0
            did = True
            break
    if did:
        rc, out = write(img, '/block', 0, b'h' * BS)
        say(rc != 0 and 'hole' in out, 'refused: a hole where the bytes would go' +
            ('' if rc != 0 else ':\n' + out.strip()))
    else:
        say(False, 'could not find the file extent record to make a hole of')

    # the extent pointed at the container's own structures: the bound that needs no tree
    base, img = fresh('e-ring')
    did = False
    for bno in leaves:
        key = struct.pack('<Q', (8 << 60) | stream) + struct.pack('<Q', 0)
        v_at, vl = find_record(img, bno, key)
        if v_at is not None and vl >= 24:
            patch(img, bno, v_at + 8, struct.pack('<Q', 2))     # into the descriptor ring
            did = True
            break
    if did:
        rc, out = write(img, '/block', 0, b'h' * BS)
        say(rc != 0, 'refused: an extent inside the checkpoint descriptor ring' +
            ('' if rc != 0 else ':\n' + out.strip()))
        say(changed_blocks(base, img) == [bno], '  and nothing but the poked block changed')
    else:
        say(False, 'could not find the file extent record to move into the ring')

    # shared blocks: the extent-reference tree saying something else refers to them too
    base, img = fresh('e-shared')
    phys = sorted(blocks_of(base, '/block'))[0]
    er = find_object(img, 0x03, 0x0f, leaf_only=True) + find_object(img, 0x02, 0x0f)
    did = False
    for bno in er:
        key = struct.pack('<Q', (2 << 60) | phys)
        v_at, vl = find_record(img, bno, key)
        if v_at is not None and vl >= 20:
            patch(img, bno, v_at + 16, struct.pack('<i', 2))    # refcnt = 2
            did = True
            break
    if did:
        rc, out = write(img, '/block', 0, b's' * BS)
        say(rc != 0 and 'more than once' in out,
            'refused: an extent the extent-reference tree says is shared' +
            ('' if rc != 0 else ':\n' + out.strip()))
    else:
        say(False, 'could not find the extent-reference record to share')

    # another owner: the same tree saying the blocks are a different stream's
    base, img = fresh('e-owner')
    did = False
    for bno in er:
        key = struct.pack('<Q', (2 << 60) | phys)
        v_at, vl = find_record(img, bno, key)
        if v_at is not None and vl >= 20:
            patch(img, bno, v_at + 8, struct.pack('<Q', stream + 1000))
            did = True
            break
    if did:
        rc, out = write(img, '/block', 0, b'o' * BS)
        say(rc != 0 and 'owner' in out,
            'refused: an extent owned by another stream' + ('' if rc != 0 else ':\n' + out.strip()))
    else:
        say(False, 'could not find the extent-reference record to give away')


def test_lying_device():
    """The last of the four bounds: a device that says the write went through and puts something
    else there.  Nothing read before the write can catch that, which is why every block is read
    back and compared before the next one goes out."""
    base, img = fresh('lying')
    data = b'\x33' * (4 * BS)
    rc, out = write(img, '/big', 0, data, corrupt_after=2)
    say(rc != 0 and 'read back' in out.lower(),
        'refused: a device that wrote something other than what it was given' +
        ('' if rc != 0 else ':\n' + out.strip()))
    diff = set(changed_blocks(base, img))
    target = blocks_of(base, '/big')
    say(diff <= target and len(diff) == 3,
        '  and it stopped at that block: %d of the file\'s own blocks changed, none other'
        % len(diff))


def test_power_failure():
    """The power out in the middle of a write, at every block in turn.  What must hold after
    each: the container is clean, the volume mounts, every other file is byte for byte what it
    was, and the blocks that changed are the target file's own and in order -- so a caller whose
    own checksum goes in last can tell that the write did not finish."""
    path, size = '/big', 1 << 20
    base, _ = fresh('pf-base')
    old = read_py_bytes(base, path)
    at, n = 8192, 12 * BS
    data = bytes((i * 7 + 3) & 0xff for i in range(n))
    want_blocks = sorted(blocks_of(base, path))[at // BS:at // BS + n // BS]
    good = True
    for k in range(0, n // BS + 1):
        _, img = fresh('pf-%d' % k)
        rc, out = write(img, path, at, data, fail_after=k)
        if k == n // BS:
            if rc != 0:
                good = say(False, '  a write stopped after every block of it should go through')
                continue
        elif rc == 0:
            good = say(False, '  a write stopped after %d blocks reported success' % k)
            continue
        ck_ok, ck = read_apfsck(img)
        if not ck_ok:
            good = say(False, '  stopped after %d blocks: apfsck is unhappy:\n%s' % (k, ck))
            continue
        diff = set(changed_blocks(base, img))
        if diff != set(want_blocks[:k]):
            good = say(False, '  stopped after %d blocks: %d blocks changed, expected %d (%s)'
                       % (k, len(diff), k, sorted(diff ^ set(want_blocks[:k]))[:6]))
            continue
        # the file itself: the first k blocks new, the rest as they were, by every reader
        want = old[:at] + data[:k * BS] + old[at + k * BS:]
        agree, bad = all_agree(img, path, want)
        if not agree:
            good = say(False, '  stopped after %d blocks: the file is neither old nor new: %s'
                       % (k, bad))
            continue
        # and nothing else in the volume moved: of every entry only this one reads differently
        moved = [l for l in (census(img) or '').splitlines()
                 if l not in (census(base) or '').splitlines()]
        if len(moved) > (1 if k else 0) or (moved and path not in moved[0]):
            good = say(False, '  stopped after %d blocks: these entries changed: %s'
                       % (k, moved[:4]))
            continue
    say(good, 'the power out after each of the %d blocks: the volume is sound every time, and '
        'exactly the blocks already written hold the new bytes' % (n // BS))


def test_real_images():
    """What the gates say about the images this port really has.  The iOS root volume is the one
    that matters: it is read through apfs.py because apfs-tool does not open a .dmg, and what it
    says about it is the reason apfs_write_volume_ok() refuses a volume by its role as well."""
    for rel in ('userland/root.apfs', 'userland/system.apfs', 'userland/flashboot.apfs'):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print('[skip] %s is not here' % rel)
            continue
        rc, out = run(TOOL, 'writable', path)
        say(rc == 0, '%s: %s' % (rel, out.strip().splitlines()[0].split(': ', 1)[-1]))

    dmg = os.path.join(ROOT, 'ios165', 'rootfs.dmg')
    if not os.path.exists(dmg):
        print('[skip] ios165/rootfs.dmg is not here')
        return
    c = apfs.Container(dmg)
    try:
        oid = next(o for o in c.nx['fs_oid'] if o)
        blk = c.blocks.get(c.omap.lookup(oid, c.xid))
        incompat, snaps = struct.unpack_from('<Q', blk, 56)[0], struct.unpack_from('<Q', blk, 216)[0]
        integrity, role = struct.unpack_from('<Q', blk, 1024)[0], struct.unpack_from('<H', blk, 964)[0]
    finally:
        c.close()
    sealed = bool(incompat & 0x20)
    print('       the iOS root volume: incompatible 0x%x (sealed: %s), snapshots %d, '
          'integrity metadata %d, role 0x%04x' % (incompat, sealed, snaps, integrity, role))
    say(not sealed and snaps == 0 and integrity == 0,
        'the iOS root volume would pass every APFS gate there is -- so the role gate is the '
        'one that refuses it, and where a write may go is what really keeps it safe')
    say(role == 0x0001, '  and it is refused: its role is the system volume')


def mtime_of(image, path):
    c = apfs.Container(image)
    try:
        vol = c.volumes()[0]
        oid, _dtype = vol.lookup(path)
        return vol.inode(oid).mod_time
    finally:
        c.close()


def test_transaction():
    """txn-probe.py: one record changed and committed as a new checkpoint, which is the next
    milestone and the measurement of what it costs.  What must hold: an independent checker
    finds the container clean afterwards, the change is there and nothing else moved, and -- the
    whole point of a transaction -- the power out at any block before the commit leaves the
    checkpoint before it live and the volume exactly as it was."""
    base, img = fresh('txn')
    rc, out = run(sys.executable, os.path.join(HERE, 'txn-probe.py'), img, '/block')
    if not say(rc == 0, 'one record changed and committed' + ('' if rc == 0 else ':\n' + out)):
        return
    total = int(out.split(' of ')[1].split()[0])
    ok, ck = read_apfsck(img)
    say(ok, '  apfsck -cw finds the committed container clean' + ('' if ok else ':\n' + ck))
    say(mtime_of(img, '/block') == mtime_of(base, '/block') + 1000000000,
        '  the changed field reads back changed')
    say(census(img) == census(base), '  and of the whole volume nothing else moved')
    good = True
    for k in range(total):
        _, part = fresh('txn-%d' % k)
        rc, out = run(sys.executable, os.path.join(HERE, 'txn-probe.py'), part, '/block',
                      '--stop-after', str(k))
        if rc != 0:
            good = say(False, '  stopped after %d blocks: the probe itself failed:\n%s' % (k, out))
            continue
        # before the commit block lands, the checkpoint before it is still the live one
        committed = mtime_of(part, '/block') != mtime_of(base, '/block')
        ck_ok, ck = read_apfsck(part, crash=False)
        if not ck_ok:
            good = say(False, '  stopped after %d blocks: apfsck is unhappy:\n%s' % (k, ck))
            continue
        if not committed and census(part) != census(base):
            good = say(False, '  stopped after %d blocks: not committed, yet the volume moved' % k)
            continue
        if committed and k < total - 2:
            good = say(False, '  stopped after %d of %d blocks and it committed anyway' % (k, total))
            continue
    say(good, 'the power out after each of the %d blocks of the transaction: the container is '
        'sound every time, and until the superblock lands the checkpoint before it is live'
        % total)

    # One after another: the descriptor ring holds four of these checkpoints and the internal
    # pool's bitmap ring sixteen, so going round them both is where a half-modelled space
    # manager would come apart.
    _, many = fresh('txn-many')
    good = True
    for i in range(20):
        rc, out = run(sys.executable, os.path.join(HERE, 'txn-probe.py'), many, '/block')
        ok, ck = read_apfsck(many)
        if rc != 0 or not ok:
            good = say(False, '  transaction %d: %s' % (i + 2, (out + ck).strip()))
            break
    say(good, 'twenty transactions one after another, each one clean: both rings go round')


TESTS = {
    'baseline': test_baseline,
    'transaction': test_transaction,
    'writes': test_writes,
    'refusals': test_refusals,
    'volume-gates': test_volume_gates,
    'extent-gates': test_extent_gates,
    'lying-device': test_lying_device,
    'power-failure': test_power_failure,
    'store': test_store,
    'store-refusals': test_store_refusals,
    'store-power': test_store_power_failure,
    'real-images': test_real_images,
}


def main(argv):
    keep = '-k' in argv
    argv = [a for a in argv if a != '-k']
    which = argv or list(TESTS)
    os.makedirs(WORK, exist_ok=True)
    for name in which:
        if name not in TESTS:
            raise SystemExit('write-tests.py: no test called %s (have: %s)'
                             % (name, ' '.join(TESTS)))
        print('--- %s' % name)
        TESTS[name]()
    print('\n%d checks, %d failed' % (done, len(failures)))
    for f in failures:
        print('  %s' % f.strip())
    if not keep:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
