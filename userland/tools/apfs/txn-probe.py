#!/usr/bin/env python3
"""Measure what a real APFS transaction costs, by doing one.

apfs_write.c replaces a file's bytes where they lie and touches no metadata at all, which is
why it needs no transaction and cannot leave the volume unmountable.  The next milestone --
changing anything at all about a file, its length, its name, its existence -- needs the whole
machinery: fresh copies of every structure from the changed record up to the container
superblock, in free space, published by one block.  Rather than guess what that costs, this
does it: one field of one inode changed and committed as a new checkpoint, on an image made by
mkapfs.py, and then apfsck and the readers are asked whether it worked.

What it is not: a writer.  It handles exactly the shape mkapfs.py produces (one volume, one
node per tree, one chunk of space, no snapshots, no clones) and says so and stops otherwise.
That makes what it does a lower bound on the cost, not an estimate of it -- the general case
has multi-level trees to split and merge, several chunks and chunk-info blocks to allocate
across, free queues to keep, and a reaper.

The one cheat, and it is named here because it is the difference between this and a filesystem:
the blocks the old checkpoint used are marked free in the chunk bitmap straight away instead of
being put on a free queue keyed by the transaction they were freed in.  The live checkpoint is
consistent -- apfsck agrees -- but the previous one is no longer safe to go back to, because
nothing stops the next transaction from allocating the blocks it still refers to.  Doing that
properly means the two free queues, which is why they are counted below as not done.

usage: txn-probe.py IMAGE PATH [--field mod_time|size] [--stop-after N]
       --stop-after N: write only the first N blocks of the transaction and leave it unfinished,
                       which is this script's power failure.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import apfs                                                     # noqa: E402
import mkapfs                                                   # noqa: E402

BS = 4096
INVALID = 0xffff
SM_BITMAP_XID_OFF = mkapfs.SM_BITMAP_XID_OFF


def die(why):
    raise SystemExit('txn-probe.py: ' + why)


def fletcher(blk):
    return mkapfs.fletcher64(bytes(blk))


def seal(blk, oid, xid, otype, subtype=0):
    struct.pack_into('<QQII', blk, 8, oid, xid, otype, subtype)
    struct.pack_into('<Q', blk, 0, fletcher(blk))


class Txn:
    """One transaction being built: blocks held in memory until they are all ready, then
    written in the order a crash has to survive -- everything the commit refers to first, the
    container superblock last."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, 'r+b')
        self.pending = []                       # (block number, bytes), in the order to write
        self.touched = {}                       # what kind of structure, how many: the measurement

    def note(self, what, n=1):
        self.touched[what] = self.touched.get(what, 0) + n

    def read(self, bno, n=1):
        self.f.seek(bno * BS)
        return bytearray(self.f.read(n * BS))

    def put(self, bno, data):
        assert len(data) % BS == 0
        self.pending.append((bno, bytes(data)))

    def commit(self, stop_after=None):
        """Out they go.  The container superblock is last and is its own commit: until it lands
        whole, with a transaction identifier larger than the last and a checksum that holds, the
        reader goes on taking the checkpoint before it."""
        n = 0
        for bno, data in self.pending:
            if stop_after is not None and n >= stop_after:
                break
            self.f.seek(bno * BS)
            self.f.write(data)
            n += 1
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()
        return n


# ---------------------------------------------------------------- reading the one shape we know

class Image:
    def __init__(self, path):
        c = apfs.Container(path)
        self.nx_blk = c.sb                                      # the live container superblock
        self.nx = c.nx
        self.xid = c.xid
        self.block_count = c.nx['block_count']
        self.desc_base = struct.unpack_from('<Q', self.nx_blk, 112)[0]
        self.desc_blocks = struct.unpack_from('<I', self.nx_blk, 104)[0]
        self.data_base = struct.unpack_from('<Q', self.nx_blk, 120)[0]
        self.data_blocks = struct.unpack_from('<I', self.nx_blk, 108)[0]
        self.desc_next = struct.unpack_from('<I', self.nx_blk, 128)[0]
        self.data_next = struct.unpack_from('<I', self.nx_blk, 132)[0]
        self.sm_oid = struct.unpack_from('<Q', self.nx_blk, 152)[0]
        self.nx_omap_bno = struct.unpack_from('<Q', self.nx_blk, 160)[0]
        self.reaper_oid = struct.unpack_from('<Q', self.nx_blk, 168)[0]
        self.next_oid = struct.unpack_from('<Q', self.nx_blk, 88)[0]
        fs = [o for o in c.nx['fs_oid'] if o]
        if len(fs) != 1:
            die('this probe knows one volume; the image has %d' % len(fs))
        self.vol_oid = fs[0]
        self.vol_bno = c.omap.lookup(self.vol_oid, c.xid)
        self.vol_blk = c.blocks.get(self.vol_bno)
        self.vol_omap_bno = struct.unpack_from('<Q', self.vol_blk, 128)[0]
        self.root_tree_oid = struct.unpack_from('<Q', self.vol_blk, 136)[0]
        vol = c.volumes()[0]
        self.vol = vol
        self.container = c

    def close(self):
        self.container.close()


def one_node_tree(blk, what):
    """The root node of a tree that is its own only node: anything else and this probe stops."""
    flags = struct.unpack_from('<H', blk, 32)[0]
    level = struct.unpack_from('<H', blk, 34)[0]
    if not (flags & 0x0001) or not (flags & 0x0002) or level != 0:
        die('%s is not a single-node tree (flags 0x%x, level %d); this probe handles only the '
            'shape mkapfs.py makes' % (what, flags, level))


def node_entries(blk):
    """Every entry of a leaf node as (key bytes, where the key is, its length, where the value
    is, its length) -- enough to change one in place.  Both shapes of table of contents: the
    file-system tree's entries are of varying size, an object map's are fixed."""
    flags, _level, nkeys = struct.unpack_from('<HHI', blk, 32)
    toc_off, toc_len = struct.unpack_from('<HH', blk, 40)
    toc = 56 + toc_off
    key_base = toc + toc_len
    root = bool(flags & 0x0001)
    val_end = BS - (40 if root else 0)
    fixed = bool(flags & 0x0004)
    if fixed:
        if not root:
            die('a fixed-size node that is not a root: this probe cannot find its key size')
        kl, vl = struct.unpack_from('<II', blk, BS - 40 + 8)
    out = []
    for i in range(nkeys):
        if fixed:
            koff, voff = struct.unpack_from('<HH', blk, toc + 4 * i)
            out.append((bytes(blk[key_base + koff:key_base + koff + kl]),
                        key_base + koff, kl, val_end - voff, vl))
            continue
        koff, kl2, voff, vl2 = struct.unpack_from('<HHHH', blk, toc + 8 * i)
        out.append((bytes(blk[key_base + koff:key_base + koff + kl2]),
                    key_base + koff, kl2, val_end - voff, vl2))
    return out


# ---------------------------------------------------------------- the space manager

class Spaceman:
    """The one chunk of space an image this small has, and the internal pool that keeps its
    bitmap.  Every field here is one the reader in bsd/miscfs/apfs does not model at all."""

    def __init__(self, txn, img):
        self.txn = txn
        self.img = img
        # where the ephemeral objects are in the data ring, from the checkpoint map
        self.cpm_bno = None
        self.maps = None
        for i in range(img.desc_blocks):
            blk = txn.read(img.desc_base + i)
            if not apfs.checksum_ok(bytes(blk)):
                continue
            otype = struct.unpack_from('<I', blk, 24)[0]
            if (otype & 0xffff) != mkapfs.T_CPMAP:
                continue
            if struct.unpack_from('<Q', blk, 16)[0] != img.xid:
                continue
            count = struct.unpack_from('<I', blk, 36)[0]
            self.cpm_bno = img.desc_base + i
            self.maps = []
            for m in range(count):
                t, st, size, _pad, _fs, oid, paddr = struct.unpack_from('<IIIIQQQ', blk, 40 + 40 * m)
                self.maps.append([t, st, size, oid, paddr])
        if self.maps is None:
            die('no checkpoint map for the live transaction')
        self.sm = None
        for t, st, size, oid, paddr in self.maps:
            if (t & 0xffff) == mkapfs.T_SPACEMAN:
                self.sm_size = size
                self.sm_bno = paddr
                self.sm = txn.read(paddr, size // BS)
        if self.sm is None:
            die('the checkpoint map names no space manager')

        self.bs, self.bpc, self.chunks_per_cib, self.cibs_per_cab = \
            struct.unpack_from('<IIII', self.sm, 32)
        (self.bc, self.chunks, self.cibs, self.cabs, self.free,
         self.cib_addr_off, _x, _y) = struct.unpack_from('<QQIIQIIQ', self.sm, 48)
        (_z, self.tx_mult, self.ip_blocks, self.ip_bm_size, self.ip_bmap_blocks,
         self.ip_bm_base, self.ip_base, _a, _b) = struct.unpack_from('<IIQIIQQQQ', self.sm, 144)
        (self.free_head, self.free_tail, self.xid_off, self.bitmap_off,
         self.free_next_off) = struct.unpack_from('<HHIII', self.sm, 320)
        if self.cibs != 1 or self.cabs != 0 or self.ip_bm_size != 1:
            die('this probe handles one chunk-info block, no chunk-info address blocks and a '
                'one-block internal-pool bitmap; this image has %d/%d/%d'
                % (self.cibs, self.cabs, self.ip_bm_size))
        self.cib_bno = struct.unpack_from('<Q', self.sm, self.cib_addr_off)[0]
        self.cib = txn.read(self.cib_bno)
        self.chunk_count = struct.unpack_from('<I', self.cib, 36)[0]
        if self.chunk_count != 1:
            die('this probe handles one chunk of space; this image has %d' % self.chunk_count)
        _xid, self.chunk_start, self.chunk_blocks, self.chunk_free, self.bmap_bno = \
            struct.unpack_from('<QQIIQ', self.cib, 40)
        self.bmap = txn.read(self.bmap_bno)
        # the internal pool's own bitmap: which of its blocks are in use
        self.ip_slot = struct.unpack_from('<H', self.sm, self.bitmap_off)[0]
        self.ip_bm = txn.read(self.ip_bm_base + self.ip_slot)
        self.freed = []
        self.allocated = []
        self.ip_allocated = []
        self.ip_freed = []

    # -- the container's blocks

    def _bit(self, bmap, i):
        return (bmap[i >> 3] >> (i & 7)) & 1

    def _set(self, bmap, i, v):
        if v:
            bmap[i >> 3] |= 1 << (i & 7)
        else:
            bmap[i >> 3] &= ~(1 << (i & 7)) & 0xff

    def alloc(self, n=1):
        out = []
        for b in range(self.chunk_start, self.chunk_start + self.chunk_blocks):
            if len(out) == n:
                break
            if not self._bit(self.bmap, b - self.chunk_start):
                self._set(self.bmap, b - self.chunk_start, 1)
                out.append(b)
        if len(out) != n:
            die('no room: %d free blocks wanted, %d found' % (n, len(out)))
        self.allocated += out
        return out

    def free_block(self, b):
        self._set(self.bmap, b - self.chunk_start, 0)
        self.freed.append(b)

    # -- the internal pool's blocks (chunk bitmaps, chunk-info blocks)

    def ip_alloc(self, n=1):
        out = []
        for i in range(self.ip_blocks):
            if len(out) == n:
                break
            if not self._bit(self.ip_bm, i):
                self._set(self.ip_bm, i, 1)
                out.append(self.ip_base + i)
        if len(out) != n:
            die('the internal pool is full: %d blocks wanted, %d found' % (n, len(out)))
        self.ip_allocated += out
        return out

    def ip_free(self, b):
        self._set(self.ip_bm, b - self.ip_base, 0)
        self.ip_freed.append(b)

    def finish(self, xid, data_at):
        """Write out everything the space manager is made of, into fresh blocks, and return
        where its own object landed in the checkpoint data ring."""
        txn = self.txn
        # the chunk bitmap and the chunk-info block, both copied inside the internal pool
        new_bmap, new_cib = self.ip_alloc(2)
        self.ip_free(self.bmap_bno)
        self.ip_free(self.cib_bno)
        in_use = sum(bin(x).count('1') for x in self.bmap[:(self.chunk_blocks + 7) // 8])
        struct.pack_into('<QQIIQ', self.cib, 40, xid, self.chunk_start, self.chunk_blocks,
                         self.chunk_blocks - in_use, new_bmap)
        seal(self.cib, new_cib, xid, mkapfs.PHYSICAL | mkapfs.T_CIB)
        txn.put(new_bmap, bytes(self.bmap))
        txn.put(new_cib, bytes(self.cib))
        txn.note('chunk bitmaps (internal pool)')
        txn.note('chunk-info blocks (internal pool)')

        # the internal pool's own bitmap, into the next slot of its ring of 16
        slot = self.free_head
        if slot == INVALID:
            die('the internal pool bitmap ring has no free slot')
        nxt = struct.unpack_from('<H', self.sm, self.free_next_off + 2 * slot)[0]
        # the slot taken leaves the free list (invalid, as every used slot is), and the slot
        # given up goes on its end, which is where the next transaction but fifteen will find it
        struct.pack_into('<H', self.sm, self.free_next_off + 2 * slot, INVALID)
        struct.pack_into('<H', self.sm, self.free_next_off + 2 * self.free_tail, self.ip_slot)
        struct.pack_into('<H', self.sm, self.free_next_off + 2 * self.ip_slot, INVALID)
        self.free_head, self.free_tail = nxt, self.ip_slot
        struct.pack_into('<HH', self.sm, 320, self.free_head, self.free_tail)
        struct.pack_into('<H', self.sm, self.bitmap_off, slot)
        struct.pack_into('<Q', self.sm, self.xid_off, xid)
        txn.put(self.ip_bm_base + slot, bytes(self.ip_bm))
        txn.note('internal-pool bitmap copies (a ring of %d)' % self.ip_bmap_blocks)

        # the space manager itself: new totals, the new chunk-info block, into the data ring
        struct.pack_into('<Q', self.sm, 48 + 24, self.chunk_blocks - in_use)      # sm_free_count
        struct.pack_into('<Q', self.sm, self.cib_addr_off, new_cib)
        seal(self.sm, self.img.sm_oid, xid, mkapfs.EPHEMERAL | mkapfs.T_SPACEMAN)
        at = self.img.data_base + data_at
        txn.put(at, bytes(self.sm))
        txn.note('the space manager object (checkpoint data ring)')
        return at, self.sm_size // BS


# ---------------------------------------------------------------- the transaction

def transact(path, file_path, field, stop_after=None):
    img = Image(path)
    txn = Txn(path)
    xid = img.xid + 1

    # -- the record to change
    oid, _dtype = img.vol.lookup(file_path) or die('%s: no such path' % file_path)
    fs_root = img.vol.omap.lookup(img.root_tree_oid, img.xid)
    fs_node = txn.read(fs_root)
    one_node_tree(fs_node, 'the file-system tree')
    key = struct.pack('<Q', (mkapfs.J_INODE << 60) | oid)
    hit = [e for e in node_entries(fs_node) if e[0] == key]
    if not hit:
        die('no inode record for %s in the one node of the tree' % file_path)
    _k, _ko, _kl, v_at, v_len = hit[0]
    if field == 'mod_time':
        was = struct.unpack_from('<Q', fs_node, v_at + 24)[0]
        struct.pack_into('<Q', fs_node, v_at + 24, was + 1000000000)
    else:
        die('this probe changes mod_time, nothing else')

    sm = Spaceman(txn, img)

    # -- fresh blocks for everything from the record up to the container superblock
    new_fs, new_vol_omap_tree, new_vol_omap, new_vol_sb, new_nx_omap_tree, new_nx_omap = sm.alloc(6)

    # the file-system tree's node: a virtual object, so the same identifier at a new address
    seal(fs_node, img.root_tree_oid, xid, mkapfs.VIRTUAL | mkapfs.T_BTREE, mkapfs.T_FSTREE)
    txn.put(new_fs, bytes(fs_node))
    txn.note('file-system tree nodes')

    # the volume's object map: the entry for that node now points at the new address.  The entry
    # is replaced rather than added -- with no snapshot, nothing needs the version before.
    vol_omap = txn.read(img.vol_omap_bno)
    vo_tree_bno = struct.unpack_from('<Q', vol_omap, 48)[0]
    vo_tree = txn.read(vo_tree_bno)
    one_node_tree(vo_tree, "the volume's object map")
    patched = 0
    for k, ko, kl, va, vl in node_entries(vo_tree):
        if struct.unpack_from('<Q', k, 0)[0] == img.root_tree_oid:
            struct.pack_into('<Q', vo_tree, ko + 8, xid)                # the key's transaction
            struct.pack_into('<Q', vo_tree, va + 8, new_fs)             # ov_paddr
            patched += 1
    if patched != 1:
        die("the volume's object map has %d entries for the tree root, not one" % patched)
    seal(vo_tree, new_vol_omap_tree, xid, mkapfs.PHYSICAL | mkapfs.T_BTREE, mkapfs.T_OMAP)
    txn.put(new_vol_omap_tree, bytes(vo_tree))
    txn.note("object-map tree nodes (volume)")
    struct.pack_into('<Q', vol_omap, 48, new_vol_omap_tree)
    seal(vol_omap, new_vol_omap, xid, mkapfs.PHYSICAL | mkapfs.T_OMAP)
    txn.put(new_vol_omap, bytes(vol_omap))
    txn.note("object-map objects (volume)")

    # the volume superblock: a virtual object too
    vol_sb = bytearray(img.vol_blk)
    struct.pack_into('<Q', vol_sb, 128, new_vol_omap)
    seal(vol_sb, img.vol_oid, xid, mkapfs.VIRTUAL | mkapfs.T_FS)
    txn.put(new_vol_sb, bytes(vol_sb))
    txn.note('volume superblocks')

    # the container's object map: the same for the volume superblock
    nx_omap = txn.read(img.nx_omap_bno)
    nxo_tree_bno = struct.unpack_from('<Q', nx_omap, 48)[0]
    nxo_tree = txn.read(nxo_tree_bno)
    one_node_tree(nxo_tree, "the container's object map")
    patched = 0
    for k, ko, kl, va, vl in node_entries(nxo_tree):
        if struct.unpack_from('<Q', k, 0)[0] == img.vol_oid:
            struct.pack_into('<Q', nxo_tree, ko + 8, xid)
            struct.pack_into('<Q', nxo_tree, va + 8, new_vol_sb)
            patched += 1
    if patched != 1:
        die("the container's object map has %d entries for the volume, not one" % patched)
    seal(nxo_tree, new_nx_omap_tree, xid, mkapfs.PHYSICAL | mkapfs.T_BTREE, mkapfs.T_OMAP)
    txn.put(new_nx_omap_tree, bytes(nxo_tree))
    txn.note('object-map tree nodes (container)')
    struct.pack_into('<Q', nx_omap, 48, new_nx_omap_tree)
    seal(nx_omap, new_nx_omap, xid, mkapfs.PHYSICAL | mkapfs.T_OMAP)
    txn.put(new_nx_omap, bytes(nx_omap))
    txn.note('object-map objects (container)')

    # what the old checkpoint used is given back (see the cheat in the docstring)
    for b in (fs_root, vo_tree_bno, img.vol_omap_bno, img.vol_bno, nxo_tree_bno, img.nx_omap_bno):
        sm.free_block(b)

    # -- the ephemeral objects, copied into the data ring.  They have to lie there in the order
    # the checkpoint map names them and with no gap, starting at the index the superblock gives:
    # that is what makes a checkpoint's data a single extent of the ring.
    eph = []
    data_index = img.data_next % img.data_blocks
    data_at = data_index
    for t, st, size, o, paddr in sm.maps:
        if (t & 0xffff) == mkapfs.T_SPACEMAN:
            at, blocks = sm.finish(xid, data_at)
            eph.append((at, t, st, size, o))
            data_at += blocks
            continue
        blk = txn.read(paddr, size // BS)
        at = img.data_base + data_at
        seal(blk, o, xid, t, st)
        txn.put(at, bytes(blk))
        eph.append((at, t, st, size, o))
        data_at += size // BS
        txn.note('ephemeral objects copied into the data ring')

    # -- the checkpoint map, then the container superblock: the commit
    desc_at = img.desc_next % img.desc_blocks
    cpm = bytearray(BS)
    struct.pack_into('<II', cpm, 32, 1, len(eph))               # the last map of the checkpoint
    for i, (at, t, st, size, o) in enumerate(eph):
        struct.pack_into('<IIIIQQQ', cpm, 40 + 40 * i, t, st, size, 0, 0, o, at)
    seal(cpm, img.desc_base + desc_at, xid, mkapfs.PHYSICAL | mkapfs.T_CPMAP)
    txn.put(img.desc_base + desc_at, bytes(cpm))
    txn.note('checkpoint maps (descriptor ring)')

    nx = bytearray(img.nx_blk)
    struct.pack_into('<Q', nx, 96, xid + 1)                     # nx_next_xid
    struct.pack_into('<Q', nx, 160, new_nx_omap)
    struct.pack_into('<IIII', nx, 128, (desc_at + 2) % img.desc_blocks,
                     data_at % img.data_blocks, desc_at, 2)
    struct.pack_into('<II', nx, 144, data_index, data_at - data_index)
    seal(nx, 1, xid, mkapfs.EPHEMERAL | mkapfs.T_NX)
    txn.put(img.desc_base + (desc_at + 1) % img.desc_blocks, bytes(nx))
    txn.note('container superblocks (the commit)')

    # Block zero holds a copy of the container superblock, which a filesystem puts back only
    # when it is unmounted cleanly: with an older copy there apfsck -c says, rightly, that the
    # volume was not unmounted cleanly, while apfsck -w still finds it sound.  It is written
    # last and is not part of the commit -- and it is the one block in all of this whose tearing
    # is not harmless, because apfs_nx_open() takes the block size and where the descriptor ring
    # is from it without checking its checksum first.
    txn.put(0, bytes(nx))
    txn.note('block zero, the copy a clean unmount leaves (not part of the commit)')

    img.close()
    written = txn.commit(stop_after)
    return xid, txn.touched, written, len(txn.pending), sm


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path, file_path = argv[0], argv[1]
    field = 'mod_time'
    stop = None
    if '--field' in argv:
        field = argv[argv.index('--field') + 1]
    if '--stop-after' in argv:
        stop = int(argv[argv.index('--stop-after') + 1])
    xid, touched, written, total, sm = transact(path, file_path, field, stop)
    print('%s: transaction %d: %d of %d blocks written%s'
          % (path, xid, written, total, ' (stopped on purpose)' if written < total else ''))
    print('  what one changed record came to:')
    for what in sorted(touched):
        print('    %-50s %d' % (what, touched[what]))
    print('    %-50s %d' % ('blocks allocated', len(sm.allocated) + len(sm.ip_allocated)))
    print('    %-50s %d' % ('blocks given back', len(sm.freed) + len(sm.ip_freed)))
    print('    %-50s %s' % ('free queues kept', 'no -- see the cheat in this script'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
