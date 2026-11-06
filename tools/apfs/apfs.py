#!/usr/bin/env python3
# Read an APFS container: the volumes in it, their directory trees, and the contents of
# files -- including the ones stored compressed.  Read only, and it never writes to the
# image.
#
# The on-disk structures are described in bsd/miscfs/apfs/apfs_format.h (exported to
# xnu/files/); the layout this file assumes is checked against that header by
# tools/apfs/run-tests.sh, so the two cannot drift apart silently.
#
# usage: apfs.py info   IMAGE
#        apfs.py ls     IMAGE [PATH]
#        apfs.py tree   IMAGE [PATH]
#        apfs.py cat    IMAGE PATH
#        apfs.py stat   IMAGE PATH
#        apfs.py census IMAGE            (walk everything, count what is in there)
# IMAGE is a raw APFS partition, or a .dmg -- the APFS partition inside it is used.
# -v VOLUME picks a volume by name or index (default: the first one with a root directory).

import os
import struct
import sys
import zlib

BLOCK0 = 0

# ---------------------------------------------------------------- object header

OBJ_HDR_SIZE = 32
OBJECT_TYPE_MASK = 0x0000FFFF
OBJ_STORAGETYPE_MASK = 0xC0000000
OBJ_VIRTUAL = 0x00000000
OBJ_EPHEMERAL = 0x80000000
OBJ_PHYSICAL = 0x40000000
OBJ_NOHEADER = 0x20000000

OBJECT_TYPE_NX_SUPERBLOCK = 0x01
OBJECT_TYPE_BTREE = 0x02
OBJECT_TYPE_BTREE_NODE = 0x03
OBJECT_TYPE_OMAP = 0x0B
OBJECT_TYPE_CHECKPOINT_MAP = 0x0C
OBJECT_TYPE_FS = 0x0D
OBJECT_TYPE_FSTREE = 0x0E

NX_MAGIC = b"NXSB"
APFS_MAGIC = b"APSB"

OID_NX_SUPERBLOCK = 1

# ---------------------------------------------------------------- b-tree

BTNODE_ROOT = 0x0001
BTNODE_LEAF = 0x0002
BTNODE_FIXED_KV_SIZE = 0x0004
BTNODE_HASHED = 0x0008
BTNODE_NOHEADER = 0x0010

BTREE_PHYSICAL = 0x00000010

BTREE_NODE_DATA_OFF = 56      # where btn_data starts, i.e. sizeof(btree_node_phys)
BTREE_INFO_SIZE = 40          # sizeof(btree_info), kept at the end of a root node

# ---------------------------------------------------------------- record kinds

OBJ_ID_MASK = 0x0FFFFFFFFFFFFFFF
OBJ_TYPE_SHIFT = 60

TYPE_SNAP_METADATA = 1
TYPE_EXTENT = 2
TYPE_INODE = 3
TYPE_XATTR = 4
TYPE_SIBLING_LINK = 5
TYPE_DSTREAM_ID = 6
TYPE_CRYPTO_STATE = 7
TYPE_FILE_EXTENT = 8
TYPE_DIR_REC = 9
TYPE_DIR_STATS = 10
TYPE_SNAP_NAME = 11
TYPE_SIBLING_MAP = 12
TYPE_FILE_INFO = 13

TYPE_NAMES = {
    1: "snap_metadata", 2: "extent", 3: "inode", 4: "xattr", 5: "sibling_link",
    6: "dstream_id", 7: "crypto_state", 8: "file_extent", 9: "dir_rec",
    10: "dir_stats", 11: "snap_name", 12: "sibling_map", 13: "file_info",
}

ROOT_DIR_INO_NUM = 2

J_DREC_LEN_MASK = 0x000003FF
J_DREC_HASH_SHIFT = 10
DREC_TYPE_MASK = 0x000F

J_FILE_EXTENT_LEN_MASK = 0x00FFFFFFFFFFFFFF

XATTR_DATA_STREAM = 0x01
XATTR_DATA_EMBEDDED = 0x02

INO_EXT_TYPE_NAME = 4
INO_EXT_TYPE_DSTREAM = 8
INO_EXT_TYPE_SPARSE_BYTES = 13
INO_EXT_TYPE_RDEV = 14

INODE_IS_SPARSE = 0x00000200
INODE_HAS_UNCOMPRESSED_SIZE = 0x00040000

S_IFMT = 0o170000
S_IFIFO, S_IFCHR, S_IFDIR, S_IFBLK = 0o010000, 0o020000, 0o040000, 0o060000
S_IFREG, S_IFLNK, S_IFSOCK = 0o100000, 0o120000, 0o140000

SYMLINK_EA_NAME = "com.apple.fs.symlink"
DECMPFS_XATTR_NAME = "com.apple.decmpfs"
RSRC_FORK_XATTR_NAME = "com.apple.ResourceFork"

DECMPFS_MAGIC = 0x636D7066
DECMPFS_BLOCK_SIZE = 65536

INCOMPAT_CASE_INSENSITIVE = 0x01
INCOMPAT_NORMALIZATION_INSENSITIVE = 0x08
INCOMPAT_SEALED_VOLUME = 0x20


class ApfsError(Exception):
    pass


# ---------------------------------------------------------------- checksum

def _fletcher64_py(buf):
    mod = 0xFFFFFFFF
    words = struct.unpack("<%dI" % (len(buf) // 4), buf)
    sum1 = sum2 = 0
    for w in words:
        sum1 += w
        sum2 += sum1
    sum1 %= mod
    sum2 %= mod
    c1 = mod - ((sum1 + sum2) % mod)
    c2 = mod - ((sum1 + c1) % mod)
    return (c2 << 32) | c1


def _make_fletcher64():
    """numpy if it is there -- this runs over every metadata block we touch."""
    try:
        import numpy as np
    except ImportError:
        return _fletcher64_py

    def f(buf):
        mod = 0xFFFFFFFF
        w = np.frombuffer(buf, dtype="<u4").astype(np.uint64)
        n = len(w)
        # sum1 = sum(w), sum2 = sum((n-i) * w[i]) -- both exact in 64 bits for a block
        sum1 = int(w.sum())
        weights = np.arange(n, 0, -1, dtype=np.uint64)
        sum2 = int((w * weights).sum())
        sum1 %= mod
        sum2 %= mod
        c1 = mod - ((sum1 + sum2) % mod)
        c2 = mod - ((sum1 + c1) % mod)
        return (c2 << 32) | c1
    return f


fletcher64 = _make_fletcher64()


def checksum_ok(block):
    """An object block carries its Fletcher-64 in the first eight bytes."""
    if len(block) % 4:
        return False
    return fletcher64(block[8:]) == int.from_bytes(block[:8], "little")


# ---------------------------------------------------------------- the image

class RawImage:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.path = path

    def read(self, off, length):
        return os.pread(self.fd, length, off)

    def close(self):
        os.close(self.fd)


class DmgImage:
    """A .dmg, read through its APFS partition so offsets look like a raw partition."""

    def __init__(self, path):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from dmg import Dmg, read_gpt, APFS_PART_TYPE
        self.dmg = Dmg(path)
        parts = [p for p in read_gpt(self.dmg) if p.type_guid == APFS_PART_TYPE]
        if not parts:
            raise ApfsError("%s: no APFS partition in the image" % path)
        self.base = parts[0].offset
        self.size = parts[0].length
        self.path = path

    def read(self, off, length):
        return self.dmg.read(self.base + off, length)

    def close(self):
        pass


def open_image(path):
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        if size > 512:
            f.seek(size - 512)
            if f.read(4) == b"koly":
                return DmgImage(path)
    return RawImage(path)


# ---------------------------------------------------------------- blocks

class Blocks:
    """Block-sized reads with a small cache, and the checksum check on every object."""

    def __init__(self, image, block_size, cache_blocks=4096):
        self.image = image
        self.block_size = block_size
        self.cache = {}
        self.cache_limit = cache_blocks
        self.reads = 0
        self.cache_hits = 0
        self.cksum_checked = 0
        self.cksum_bad = 0

    def raw(self, addr, count=1):
        return self.image.read(addr * self.block_size, count * self.block_size)

    def get(self, addr, verify=True):
        b = self.cache.get(addr)
        if b is not None:
            self.cache_hits += 1
            return b
        b = self.raw(addr)
        if len(b) != self.block_size:
            raise ApfsError("short read at block %d" % addr)
        if verify:
            self.cksum_checked += 1
            if not checksum_ok(b):
                self.cksum_bad += 1
                raise ApfsError("block %d: bad Fletcher-64 checksum" % addr)
        if len(self.cache) >= self.cache_limit:
            # cheap enough: drop a chunk of the cache rather than track an LRU order
            for k in list(self.cache)[:self.cache_limit // 4]:
                del self.cache[k]
        self.cache[addr] = b
        return b


def obj_hdr(block):
    oid, xid, otype, osub = struct.unpack_from("<QQII", block, 8)
    return oid, xid, otype, osub


# ---------------------------------------------------------------- b-tree

class Node:
    __slots__ = ("addr", "blk", "flags", "level", "nkeys", "toc_off", "toc_len",
                 "key_base", "val_end", "fixed", "key_size", "val_size")

    def __init__(self, addr, blk, block_size, key_size=0, val_size=0):
        self.addr = addr
        self.blk = blk
        (self.flags, self.level, self.nkeys) = struct.unpack_from("<HHI", blk, 32)
        ts_off, ts_len = struct.unpack_from("<HH", blk, 40)
        self.toc_off = BTREE_NODE_DATA_OFF + ts_off
        self.toc_len = ts_len
        # keys start right after the table of contents, values end at the end of the
        # block -- except in a root node, where btree_info sits in the last bytes
        self.key_base = self.toc_off + self.toc_len
        self.val_end = len(blk) - (BTREE_INFO_SIZE if (self.flags & BTNODE_ROOT) else 0)
        self.fixed = bool(self.flags & BTNODE_FIXED_KV_SIZE)
        self.key_size = key_size
        self.val_size = val_size

    @property
    def is_leaf(self):
        return bool(self.flags & BTNODE_LEAF)

    def entry(self, i):
        """(key bytes, value bytes) of entry i."""
        if self.fixed:
            koff, voff = struct.unpack_from("<HH", self.blk, self.toc_off + 4 * i)
            klen = self.key_size
            vlen = self.val_size if self.is_leaf else 8
        else:
            koff, klen, voff, vlen = struct.unpack_from(
                "<HHHH", self.blk, self.toc_off + 8 * i)
        k = self.blk[self.key_base + koff:self.key_base + koff + klen]
        if voff == 0xFFFF:              # a ghost: a key with no value
            return k, b""
        v = self.blk[self.val_end - voff:self.val_end - voff + vlen]
        return k, v

    def key(self, i):
        if self.fixed:
            koff = struct.unpack_from("<H", self.blk, self.toc_off + 4 * i)[0]
            klen = self.key_size
        else:
            koff, klen = struct.unpack_from("<HH", self.blk, self.toc_off + 8 * i)
        return self.blk[self.key_base + koff:self.key_base + koff + klen]

    def child_oid(self, i):
        _k, v = self.entry(i)
        return struct.unpack_from("<Q", v, 0)[0]


class Btree:
    """
    One B-tree.  A tree is either physical -- a node's identifier is its block address --
    or virtual, and then identifiers are resolved through an object map.
    """

    def __init__(self, blocks, root_oid, resolve=None, name="btree"):
        self.blocks = blocks
        self.name = name
        self.resolve = resolve
        self.root_addr = self._addr(root_oid, physical_hint=True)
        rb = blocks.get(self.root_addr)
        (bt_flags, bt_node_size, bt_key_size, bt_val_size) = struct.unpack_from(
            "<IIII", rb, len(rb) - BTREE_INFO_SIZE)
        self.bt_flags = bt_flags
        self.node_size = bt_node_size
        self.key_size = bt_key_size
        self.val_size = bt_val_size
        (self.longest_key, self.longest_val, self.key_count, self.node_count) = \
            struct.unpack_from("<IIQQ", rb, len(rb) - BTREE_INFO_SIZE + 16)
        self.physical = bool(bt_flags & BTREE_PHYSICAL)
        self.root = Node(self.root_addr, rb, blocks.block_size, bt_key_size, bt_val_size)
        self.nodes_read = 0

    def _addr(self, oid, physical_hint=False):
        if physical_hint and self.resolve is None:
            return oid
        if self.resolve is None:
            return oid
        if getattr(self, "physical", None):
            return oid
        return self.resolve(oid)

    def node(self, oid):
        addr = oid if (self.physical or self.resolve is None) else self.resolve(oid)
        blk = self.blocks.get(addr)
        self.nodes_read += 1
        return Node(addr, blk, self.blocks.block_size, self.key_size, self.val_size)

    def child(self, node, i):
        return self.node(node.child_oid(i))

    def iter_from(self, less):
        """
        Every entry from the first key for which less(key) is false, in key order.
        `less` says whether a key sorts before the point we want to start at; passing
        a function that is always false walks the whole tree.
        """
        path = []
        node = self.root
        while not node.is_leaf:
            i = 0
            for j in range(node.nkeys):
                if less(node.key(j)):
                    i = j
                else:
                    break
            path.append((node, i))
            node = self.child(node, i)
        i = 0
        while i < node.nkeys and less(node.key(i)):
            i += 1
        while True:
            while i < node.nkeys:
                yield node.entry(i)
                i += 1
            # done with this leaf: step the path to the next one
            while path and path[-1][1] + 1 >= path[-1][0].nkeys:
                path.pop()
            if not path:
                return
            pnode, pi = path[-1]
            path[-1] = (pnode, pi + 1)
            node = self.child(pnode, pi + 1)
            while not node.is_leaf:
                path.append((node, 0))
                node = self.child(node, 0)
            i = 0

    def iter_all(self):
        return self.iter_from(lambda k: False)


# ---------------------------------------------------------------- object map

class Omap:
    """An object map: (object identifier, transaction) -> block address."""

    def __init__(self, blocks, omap_addr):
        blk = blocks.get(omap_addr)
        oid, xid, otype, _sub = obj_hdr(blk)
        if (otype & OBJECT_TYPE_MASK) != OBJECT_TYPE_OMAP:
            raise ApfsError("block %d is not an object map (type 0x%x)" % (omap_addr, otype))
        (self.flags, self.snap_count, self.tree_type, self.snapshot_tree_type) = \
            struct.unpack_from("<IIII", blk, 32)
        (self.tree_oid, self.snapshot_tree_oid) = struct.unpack_from("<QQ", blk, 48)
        self.tree = Btree(blocks, self.tree_oid, name="omap")
        self.cache = {}

    def lookup(self, oid, xid=None):
        """
        Where an object lives: the newest version at or before xid (the newest of all, if
        xid is None).  The map is keyed by (identifier, transaction), so the versions of
        one object are a run; we take the last one that is not newer than we asked for.
        """
        hit = self.cache.get(oid)
        if hit is not None:
            return hit
        want_xid = (1 << 64) - 1 if xid is None else xid

        def less(k):
            o, _x = struct.unpack_from("<QQ", k, 0)
            return o < oid

        best = None
        for k, v in self.tree.iter_from(less):
            o, x = struct.unpack_from("<QQ", k, 0)
            if o != oid:
                break
            if x <= want_xid:
                best = v
        if best is None:
            raise ApfsError("object %d is not in the object map" % oid)
        _flags, _size, paddr = struct.unpack_from("<IIq", best, 0)
        self.cache[oid] = paddr
        return paddr

    def resolver(self):
        return lambda oid: self.lookup(oid)


# ---------------------------------------------------------------- container

class Container:
    def __init__(self, path):
        self.image = open_image(path)
        self.path = path
        head = self.image.read(0, 4096)
        if head[32:36] != NX_MAGIC:
            raise ApfsError("%s: no NXSB magic at offset 32 -- not an APFS container" % path)
        block_size = struct.unpack_from("<I", head, 36)[0]
        if not (4096 <= block_size <= 65536) or (block_size & (block_size - 1)):
            raise ApfsError("%s: block size %d makes no sense" % (path, block_size))
        self.blocks = Blocks(self.image, block_size)
        self.block_size = block_size
        self.sb = self._newest_superblock()
        self.nx = self._parse_nx(self.sb)
        self.omap = Omap(self.blocks, self.nx["omap_oid"])

    def _parse_nx(self, blk):
        d = {}
        (d["magic"], d["block_size"], d["block_count"]) = struct.unpack_from("<IIQ", blk, 32)
        (d["features"], d["ro_compat"], d["incompat"]) = struct.unpack_from("<QQQ", blk, 48)
        d["uuid"] = blk[72:88]
        (d["next_oid"], d["next_xid"]) = struct.unpack_from("<QQ", blk, 88)
        (d["xp_desc_blocks"], d["xp_data_blocks"]) = struct.unpack_from("<II", blk, 104)
        (d["xp_desc_base"], d["xp_data_base"]) = struct.unpack_from("<qq", blk, 112)
        (d["xp_desc_next"], d["xp_data_next"], d["xp_desc_index"], d["xp_desc_len"],
         d["xp_data_index"], d["xp_data_len"]) = struct.unpack_from("<IIIIII", blk, 128)
        (d["spaceman_oid"], d["omap_oid"], d["reaper_oid"]) = struct.unpack_from("<QQQ", blk, 152)
        d["max_file_systems"] = struct.unpack_from("<I", blk, 180)[0]
        d["fs_oid"] = list(struct.unpack_from("<100Q", blk, 184))
        _, _, _, d["xid"] = obj_hdr(blk)
        return d

    def _newest_superblock(self):
        """
        Block zero's copy can be stale.  The live container superblock is the one with the
        largest transaction identifier in the checkpoint descriptor area, so read block
        zero only to find that area, then search it.
        """
        b0 = self.blocks.get(BLOCK0, verify=False)
        if not checksum_ok(b0):
            raise ApfsError("the container superblock in block 0 fails its checksum")
        base = struct.unpack_from("<q", b0, 112)[0]
        count = struct.unpack_from("<I", b0, 104)[0]
        best, best_xid = b0, obj_hdr(b0)[1]
        self.checkpoints_seen = 0
        for i in range(count):
            blk = self.blocks.raw(base + i)
            if len(blk) != self.block_size or not checksum_ok(blk):
                continue
            oid, xid, otype, _sub = obj_hdr(blk)
            if (otype & OBJECT_TYPE_MASK) != OBJECT_TYPE_NX_SUPERBLOCK:
                continue
            if blk[32:36] != NX_MAGIC or oid != OID_NX_SUPERBLOCK:
                continue
            self.checkpoints_seen += 1
            if xid > best_xid:
                best, best_xid = blk, xid
        self.xid = best_xid
        return best

    def volumes(self):
        out = []
        for i, oid in enumerate(self.nx["fs_oid"]):
            if not oid:
                continue
            try:
                addr = self.omap.lookup(oid, self.xid)
                blk = self.blocks.get(addr)
            except ApfsError:
                continue
            if blk[32:36] != APFS_MAGIC:
                continue
            out.append(Volume(self, i, oid, addr, blk))
        return out

    def close(self):
        self.image.close()


# ---------------------------------------------------------------- volume

class Volume:
    def __init__(self, container, index, oid, addr, blk):
        self.c = container
        self.index = index
        self.oid = oid
        self.addr = addr
        b = blk
        (self.magic, self.fs_index) = struct.unpack_from("<II", b, 32)
        (self.features, self.ro_compat, self.incompat) = struct.unpack_from("<QQQ", b, 40)
        self.unmount_time = struct.unpack_from("<Q", b, 64)[0]
        (self.reserve_blocks, self.quota_blocks, self.alloc_count) = \
            struct.unpack_from("<QQQ", b, 72)
        # apfs_meta_crypto is 20 bytes, packed, right after apfs_fs_alloc_count
        off = 96 + 20
        (self.root_tree_type, self.extentref_tree_type, self.snap_meta_tree_type) = \
            struct.unpack_from("<III", b, off)
        off += 12
        (self.omap_oid, self.root_tree_oid, self.extentref_tree_oid,
         self.snap_meta_tree_oid) = struct.unpack_from("<QQQQ", b, off)
        off += 32
        (self.revert_to_xid, self.revert_to_sblock_oid) = struct.unpack_from("<QQ", b, off)
        off += 16
        self.next_obj_id = struct.unpack_from("<Q", b, off)[0]
        off += 8
        (self.num_files, self.num_directories, self.num_symlinks,
         self.num_other, self.num_snapshots) = struct.unpack_from("<QQQQQ", b, off)
        off += 40
        (self.blocks_alloced, self.blocks_freed) = struct.unpack_from("<QQ", b, off)
        off += 16
        self.vol_uuid = b[off:off + 16]
        off += 16
        self.last_mod_time = struct.unpack_from("<Q", b, off)[0]
        off += 8
        self.fs_flags = struct.unpack_from("<Q", b, off)[0]
        off += 8
        off += 48 * 9                           # apfs_formatted_by + apfs_modified_by[8]
        self.volname = b[off:off + 256].split(b"\0", 1)[0].decode("utf-8", "replace")
        off += 256
        self.next_doc_id = struct.unpack_from("<I", b, off)[0]
        off += 4
        self.role = struct.unpack_from("<H", b, off)[0]
        self.xid = obj_hdr(b)[1]
        self._omap = None
        self._tree = None

    @property
    def case_insensitive(self):
        return bool(self.incompat & INCOMPAT_CASE_INSENSITIVE)

    @property
    def sealed(self):
        return bool(self.incompat & INCOMPAT_SEALED_VOLUME)

    @property
    def omap(self):
        if self._omap is None:
            self._omap = Omap(self.c.blocks, self.omap_oid)
        return self._omap

    @property
    def tree(self):
        if self._tree is None:
            self._tree = Btree(self.c.blocks, self.root_tree_oid,
                               resolve=self.omap.resolver(), name="fstree")
        return self._tree

    # -------- records

    def records(self, obj_id, rec_type):
        """
        Every record of one kind belonging to one object, in key order.

        A file-system tree sorts by object identifier first and record kind second, so the
        records of one (object, kind) are a contiguous run.  That is all the ordering this
        needs to know -- how keys of the same kind sort among themselves (by name, by hash,
        by offset) differs per kind and is left to the caller, which just reads the run.
        """
        want = (obj_id, rec_type)

        def less(k):
            v = struct.unpack_from("<Q", k, 0)[0]
            return (v & OBJ_ID_MASK, v >> OBJ_TYPE_SHIFT) < want

        for k, v in self.tree.iter_from(less):
            x = struct.unpack_from("<Q", k, 0)[0]
            if (x & OBJ_ID_MASK, x >> OBJ_TYPE_SHIFT) != want:
                return
            yield k, v

    def inode(self, obj_id):
        for _k, v in self.records(obj_id, TYPE_INODE):
            return Inode(self, obj_id, v)
        return None

    def dir_entries(self, obj_id):
        """(name, file_id, d_type) for each entry of a directory."""
        out = []
        for k, v in self.records(obj_id, TYPE_DIR_REC):
            # iOS volumes use the hashed key: a 32-bit field holding the name length and
            # a hash of the name, then the name itself
            nlh = struct.unpack_from("<I", k, 8)[0]
            name_len = nlh & J_DREC_LEN_MASK
            name = k[12:12 + name_len].split(b"\0", 1)[0]
            file_id, _date, flags = struct.unpack_from("<QQH", v, 0)
            out.append((name.decode("utf-8", "surrogateescape"), file_id,
                        flags & DREC_TYPE_MASK))
        return out

    def xattrs(self, obj_id):
        """{name: (flags, xdata bytes)} of one object's extended attributes."""
        out = {}
        for k, v in self.records(obj_id, TYPE_XATTR):
            name_len = struct.unpack_from("<H", k, 8)[0]
            name = k[10:10 + name_len].split(b"\0", 1)[0].decode("utf-8", "replace")
            flags, xdata_len = struct.unpack_from("<HH", v, 0)
            out[name] = (flags, v[4:4 + xdata_len])
        return out

    def extents(self, stream_id):
        """(logical offset, length, physical block) for one data stream, in order."""
        out = []
        for k, v in self.records(stream_id, TYPE_FILE_EXTENT):
            logical = struct.unpack_from("<Q", k, 8)[0]
            len_and_flags, phys, _crypto = struct.unpack_from("<QQQ", v, 0)
            out.append((logical, len_and_flags & J_FILE_EXTENT_LEN_MASK, phys))
        out.sort()
        return out

    # -------- reading a data stream

    def read_stream(self, stream_id, size):
        """
        The bytes of a data stream.  Gaps between extents, and extents whose physical
        block is zero, are holes and read as zeros -- that is how a sparse file is stored.
        """
        bs = self.c.block_size
        out = bytearray(size)
        for logical, length, phys in self.extents(stream_id):
            if logical >= size:
                continue
            n = min(length, size - logical)
            if phys == 0:
                continue                    # a hole; the buffer is already zero
            nblocks = (length + bs - 1) // bs
            data = self.c.blocks.raw(phys, nblocks)
            out[logical:logical + n] = data[:n]
        return bytes(out)

    def xattr_bytes(self, obj_id, name, xattrs=None):
        """One attribute's value, whether it is stored inline or in its own stream."""
        xs = self.xattrs(obj_id) if xattrs is None else xattrs
        if name not in xs:
            return None
        flags, xdata = xs[name]
        if flags & XATTR_DATA_EMBEDDED:
            return xdata
        if flags & XATTR_DATA_STREAM:
            xattr_obj_id, size = struct.unpack_from("<QQ", xdata, 0)
            return self.read_stream(xattr_obj_id, size)
        return xdata

    # -------- paths

    def lookup(self, path):
        """(object identifier, d_type) for a path, or None. Case folded if the volume is."""
        oid = ROOT_DIR_INO_NUM
        dtype = 4
        for part in [p for p in path.split("/") if p and p != "."]:
            ents = self.dir_entries(oid)
            hit = None
            for name, fid, t in ents:
                if name == part:
                    hit = (fid, t)
                    break
            if hit is None and self.case_insensitive:
                lp = part.lower()
                for name, fid, t in ents:
                    if name.lower() == lp:
                        hit = (fid, t)
                        break
            if hit is None:
                return None
            oid, dtype = hit
        return oid, dtype


# ---------------------------------------------------------------- inode

def parse_xfields(buf):
    """The extended fields at the tail of an inode or directory entry: {type: bytes}."""
    if len(buf) < 4:
        return {}
    num, used = struct.unpack_from("<HH", buf, 0)
    out = {}
    hdr = 4 + 4 * num
    if hdr > len(buf):
        return {}
    off = hdr
    for i in range(num):
        xtype, _xflags, xsize = struct.unpack_from("<BBH", buf, 4 + 4 * i)
        out[xtype] = buf[off:off + xsize]
        off += (xsize + 7) & ~7            # each field is padded out to eight bytes
    return out


class Inode:
    def __init__(self, vol, obj_id, v):
        self.vol = vol
        self.obj_id = obj_id
        (self.parent_id, self.private_id) = struct.unpack_from("<QQ", v, 0)
        (self.create_time, self.mod_time, self.change_time, self.access_time) = \
            struct.unpack_from("<QQQQ", v, 16)
        self.internal_flags = struct.unpack_from("<Q", v, 48)[0]
        self.nchildren = struct.unpack_from("<i", v, 56)[0]
        self.nlink = self.nchildren
        (self.protection_class, self.write_gen, self.bsd_flags) = \
            struct.unpack_from("<III", v, 60)
        (self.owner, self.group) = struct.unpack_from("<II", v, 72)
        (self.mode, _pad) = struct.unpack_from("<HH", v, 80)
        self.uncompressed_size = struct.unpack_from("<Q", v, 84)[0]
        self.xf = parse_xfields(v[92:])
        self._xattrs = None
        self._dstream = None
        if INO_EXT_TYPE_DSTREAM in self.xf:
            d = self.xf[INO_EXT_TYPE_DSTREAM]
            (size, alloced, _crypto, written, read) = struct.unpack_from("<QQQQQ", d, 0)
            self._dstream = (size, alloced)

    @property
    def xattrs(self):
        if self._xattrs is None:
            self._xattrs = self.vol.xattrs(self.obj_id)
        return self._xattrs

    @property
    def ifmt(self):
        return self.mode & S_IFMT

    @property
    def is_dir(self):
        return self.ifmt == S_IFDIR

    @property
    def is_reg(self):
        return self.ifmt == S_IFREG

    @property
    def is_link(self):
        return self.ifmt == S_IFLNK

    @property
    def is_sparse(self):
        return bool(self.internal_flags & INODE_IS_SPARSE)

    @property
    def rdev(self):
        d = self.xf.get(INO_EXT_TYPE_RDEV)
        return struct.unpack_from("<I", d, 0)[0] if d else 0

    @property
    def decmpfs(self):
        """The com.apple.decmpfs attribute's value, or None if the file is not compressed."""
        return self.vol.xattr_bytes(self.obj_id, DECMPFS_XATTR_NAME, self.xattrs)

    @property
    def size(self):
        """The logical size: what stat would report."""
        hdr = self.decmpfs
        if hdr is not None and len(hdr) >= 16:
            magic, _ctype, usize = struct.unpack_from("<IIQ", hdr, 0)
            if magic == DECMPFS_MAGIC:
                return usize
        if self.is_link:
            t = self.symlink_target_bytes()
            return len(t) if t is not None else 0
        if self._dstream:
            return self._dstream[0]
        return 0

    def symlink_target_bytes(self):
        t = self.vol.xattr_bytes(self.obj_id, SYMLINK_EA_NAME, self.xattrs)
        if t is None:
            return None
        return t.split(b"\0", 1)[0]

    def contents(self):
        """The file's bytes, decompressing them if that is how they are stored."""
        if self.is_link:
            return self.symlink_target_bytes() or b""
        hdr = self.decmpfs
        if hdr is not None and len(hdr) >= 16 and \
                struct.unpack_from("<I", hdr, 0)[0] == DECMPFS_MAGIC:
            return self.decompress(hdr)
        if self._dstream is None:
            return b""
        return self.vol.read_stream(self.private_id, self._dstream[0])

    def decompress(self, hdr):
        magic, ctype, usize = struct.unpack_from("<IIQ", hdr, 0)
        attr = hdr[16:]
        if ctype in (3, 9, 11):                 # the bytes are in the attribute itself
            return decmpfs_attr(ctype, attr, usize)
        if ctype in (4, 10, 12):                # the bytes are in the resource fork
            rsrc = self.vol.xattr_bytes(self.obj_id, RSRC_FORK_XATTR_NAME, self.xattrs)
            if rsrc is None:
                raise ApfsError("inode %d: compression type %d but no resource fork"
                                % (self.obj_id, ctype))
            return decmpfs_rsrc(ctype, rsrc, usize)
        raise ApfsError("inode %d: unknown decmpfs compression type %d"
                        % (self.obj_id, ctype))


def _inflate(buf, out_len):
    """
    One zlib stream.  A leading 0xff is decmpfs's way of saying "this piece did not
    compress, the rest is the bytes as they are".
    """
    if buf[:1] == b"\xff":
        return buf[1:1 + out_len]
    return zlib.decompress(buf)


def decmpfs_attr(ctype, attr, usize):
    """
    The contents sit in the com.apple.decmpfs attribute itself, right after the header.

    Type 9 means they were stored as they are, with one marker byte in front.  All 23947
    type-9 files in the iOS 16.5 root file system have 0xcc there and exactly usize bytes
    after it, which is how the marker's length was settled -- the attribute is taken as
    bare only if it is already exactly usize bytes long.
    """
    if ctype == 9:
        if len(attr) == usize:
            return attr
        return attr[1:1 + usize]
    if ctype == 3:
        return _inflate(attr, usize)[:usize]
    if ctype == 11:
        import lzfse
        if attr[:1] == b"\xff":
            return attr[1:1 + usize]
        return lzfse.decompress(attr)[:usize]
    raise ApfsError("compression type %d in the attribute is not supported" % ctype)


def decmpfs_rsrc(ctype, rsrc, usize):
    """
    The contents are in the com.apple.ResourceFork attribute, cut into 64 KB pieces, each
    compressed on its own.

    The fork is a real HFS-Plus resource fork: sixteen big-endian bytes giving the offset
    and the length of the resource data and of the resource map.  At that data offset sits
    a four-byte big-endian length, and the piece table starts right after it -- a
    little-endian count, then an (offset, length) pair per piece, the offsets measured
    from the start of the table.  The table is not at a fixed place: the data offset is
    256 in every file here, so the table lands at 260, but it has to be read rather than
    assumed.
    """
    if len(rsrc) < 16:
        raise ApfsError("resource fork too short (%d bytes)" % len(rsrc))
    data_off, _map_off, _data_len, _map_len = struct.unpack_from(">IIII", rsrc, 0)
    base = data_off + 4
    if base + 4 > len(rsrc):
        raise ApfsError("resource fork: data offset %d is past the end of the fork "
                        "(%d bytes)" % (data_off, len(rsrc)))
    nblocks = struct.unpack_from("<I", rsrc, base)[0]
    want = (usize + DECMPFS_BLOCK_SIZE - 1) // DECMPFS_BLOCK_SIZE
    if nblocks != want:
        raise ApfsError("resource fork says %d pieces, %d bytes needs %d"
                        % (nblocks, usize, want))
    out = bytearray()
    for i in range(nblocks):
        off, length = struct.unpack_from("<II", rsrc, base + 4 + 8 * i)
        piece = rsrc[base + off:base + off + length]
        n = min(DECMPFS_BLOCK_SIZE, usize - len(out))
        if ctype == 4:
            out += _inflate(piece, n)[:n]
        elif ctype == 10:
            out += piece[:n]
        elif ctype == 12:
            import lzfse
            out += (piece[1:1 + n] if piece[:1] == b"\xff"
                    else lzfse.decompress(piece)[:n])
        else:
            raise ApfsError("compression type %d in the resource fork is not supported"
                            % ctype)
    if len(out) != usize:
        raise ApfsError("decompressed %d bytes, the header says %d" % (len(out), usize))
    return bytes(out)


# ---------------------------------------------------------------- walking

DT_CHAR = {1: "p", 2: "c", 4: "d", 6: "b", 8: "-", 10: "l", 12: "s", 14: "w"}


def walk(vol, oid, path="", depth=0, max_depth=None):
    """Depth-first over a directory tree, yielding (path, name, file_id, d_type)."""
    if max_depth is not None and depth > max_depth:
        return
    for name, fid, dt in sorted(vol.dir_entries(oid)):
        p = path + "/" + name
        yield p, name, fid, dt
        if dt == 4:
            yield from walk(vol, fid, p, depth + 1, max_depth)


# ---------------------------------------------------------------- commands

def pick_volume(c, want):
    vols = c.volumes()
    if not vols:
        raise ApfsError("no volumes in this container")
    if want is None:
        for v in vols:
            if v.inode(ROOT_DIR_INO_NUM) is not None:
                return v
        return vols[0]
    for v in vols:
        if v.volname == want or str(v.index) == str(want):
            return v
    raise ApfsError("no volume named %r (have: %s)"
                    % (want, ", ".join(v.volname for v in vols)))


def cmd_info(path, want):
    c = Container(path)
    n = c.nx
    print("%s: APFS container, block size %d, %d blocks (%.2f GB)"
          % (path, c.block_size, n["block_count"], n["block_count"] * c.block_size / 1e9))
    print("  uuid              %s" % _uuid(n["uuid"]))
    print("  newest checkpoint xid %d (%d container superblocks in the descriptor area)"
          % (c.xid, c.checkpoints_seen))
    print("  checkpoint desc   base %d, %d blocks" % (n["xp_desc_base"], n["xp_desc_blocks"]))
    print("  container omap    block %d" % n["omap_oid"])
    print("  features 0x%x  ro-compat 0x%x  incompatible 0x%x"
          % (n["features"], n["ro_compat"], n["incompat"]))
    vols = c.volumes()
    print("  %d volumes:" % len(vols))
    for v in vols:
        print("    [%d] %-22s role 0x%04x  xid %d" % (v.index, v.volname, v.role, v.xid))
        print("         files %d  dirs %d  symlinks %d  other %d  snapshots %d"
              % (v.num_files, v.num_directories, v.num_symlinks, v.num_other,
                 v.num_snapshots))
        print("         incompatible 0x%x%s%s  fs flags 0x%x"
              % (v.incompat,
                 " case-insensitive" if v.case_insensitive else "",
                 " sealed" if v.sealed else "", v.fs_flags))
        print("         omap block %d  root tree oid %d  used %d blocks (%.2f GB)"
              % (v.omap_oid, v.root_tree_oid, v.alloc_count,
                 v.alloc_count * c.block_size / 1e9))
    v = pick_volume(c, want)
    t = v.tree
    print("  volume %r file-system tree: %d records, %d nodes, flags 0x%x, "
          "longest key %d, longest value %d"
          % (v.volname, t.key_count, t.node_count, t.bt_flags, t.longest_key,
             t.longest_val))
    print("  blocks read %d, cache hits %d, checksums verified %d, bad %d"
          % (c.blocks.cksum_checked, c.blocks.cache_hits, c.blocks.cksum_checked,
             c.blocks.cksum_bad))
    c.close()


def _uuid(b):
    h = b.hex()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def cmd_ls(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path on volume %r" % (where, v.volname))
    oid, dt = hit
    if dt != 4:
        _print_one(v, where.rsplit("/", 1)[-1] or "/", oid)
        c.close()
        return
    for name, fid, t in sorted(v.dir_entries(oid)):
        _print_one(v, name, fid)
    c.close()


def _print_one(v, name, fid):
    ino = v.inode(fid)
    if ino is None:
        print("?         %20s  %s  (no inode record)" % ("", name))
        return
    kind = DT_CHAR.get({S_IFDIR: 4, S_IFREG: 8, S_IFLNK: 10, S_IFIFO: 1, S_IFCHR: 2,
                        S_IFBLK: 6, S_IFSOCK: 12}.get(ino.ifmt, 0), "?")
    extra = ""
    if ino.is_link:
        extra = " -> " + (ino.symlink_target_bytes() or b"").decode("utf-8", "replace")
    cmp_ = ""
    h = ino.decmpfs
    if h is not None and len(h) >= 16:
        cmp_ = " [decmpfs %d]" % struct.unpack_from("<I", h, 4)[0]
    if ino.is_sparse:
        cmp_ += " [sparse]"
    print("%s%s %4d:%-4d %12d  %-8d %s%s%s"
          % (kind, _modestr(ino.mode), ino.owner, ino.group, ino.size, fid, name,
             extra, cmp_))


def _modestr(mode):
    s = ""
    for who, special, mark in ((6, 0o4000, "s"), (3, 0o2000, "s"), (0, 0o1000, "t")):
        s += "r" if mode & (0o4 << who) else "-"
        s += "w" if mode & (0o2 << who) else "-"
        x = mode & (0o1 << who)
        # set-user-ID, set-group-ID and sticky, as ls shows them (upper case without the x under them)
        s += (mark if x else mark.upper()) if mode & special else ("x" if x else "-")
    return s


def cmd_tree(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    n = 0
    for p, _name, _fid, dt in walk(v, hit[0], where.rstrip("/")):
        print(("%s/" % p) if dt == 4 else p)
        n += 1
    print("# %d entries" % n, file=sys.stderr)
    c.close()


def cmd_cat(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    ino = v.inode(hit[0])
    sys.stdout.buffer.write(ino.contents())
    c.close()


def cmd_stat(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    ino = v.inode(hit[0])
    print("path        %s" % where)
    print("inode       %d  parent %d  private %d" % (ino.obj_id, ino.parent_id,
                                                     ino.private_id))
    print("mode        %06o (%s)  uid %d  gid %d" % (ino.mode, _modestr(ino.mode),
                                                     ino.owner, ino.group))
    print("size        %d" % ino.size)
    print("nlink/nchildren %d" % ino.nchildren)
    print("flags       internal 0x%x  bsd 0x%x%s" % (ino.internal_flags, ino.bsd_flags,
                                                     "  sparse" if ino.is_sparse else ""))
    if ino._dstream:
        print("dstream     size %d  allocated %d" % ino._dstream)
    h = ino.decmpfs
    if h is not None and len(h) >= 16:
        magic, ctype, usize = struct.unpack_from("<IIQ", h, 0)
        print("decmpfs     type %d  uncompressed %d  attribute %d bytes"
              % (ctype, usize, len(h)))
    xs = ino.xattrs
    if xs:
        print("xattrs      %s" % ", ".join("%s(%d,flags 0x%x)" % (k, len(d), f)
                                           for k, (f, d) in sorted(xs.items())))
    ex = v.extents(ino.private_id)
    if ex:
        print("extents     %d:" % len(ex))
        for logical, length, phys in ex[:16]:
            print("  logical %12d  len %9d  block %d%s"
                  % (logical, length, phys, "  (hole)" if phys == 0 else ""))
        if len(ex) > 16:
            print("  ... %d more" % (len(ex) - 16))
    if ino.is_link:
        print("symlink     -> %s" % (ino.symlink_target_bytes() or b"").decode(
            "utf-8", "replace"))
    c.close()


def cmd_census(path, want):
    """Walk the whole tree and count what is in it: the quickest honest sanity check."""
    c = Container(path)
    v = pick_volume(c, want)
    counts = {}
    cmp_types = {}
    total = 0
    dirs = files = links = other = 0
    bytes_logical = 0
    for p, _name, fid, dt in walk(v, ROOT_DIR_INO_NUM):
        total += 1
        counts[dt] = counts.get(dt, 0) + 1
        if dt == 4:
            dirs += 1
            continue
        ino = v.inode(fid)
        if ino is None:
            other += 1
            continue
        if ino.is_link:
            links += 1
            continue
        if not ino.is_reg:
            other += 1
            continue
        files += 1
        h = ino.decmpfs
        if h is not None and len(h) >= 16:
            ct = struct.unpack_from("<I", h, 4)[0]
            cmp_types[ct] = cmp_types.get(ct, 0) + 1
        bytes_logical += ino.size
        if total % 20000 == 0:
            sys.stderr.write("\r  %d entries..." % total)
            sys.stderr.flush()
    sys.stderr.write("\r")
    print("volume %r: %d directory entries" % (v.volname, total))
    print("  directories %d  regular %d  symlinks %d  other %d" % (dirs, files, links, other))
    print("  d_type counts: %s" % ", ".join("%s=%d" % (DT_CHAR.get(k, k), n)
                                            for k, n in sorted(counts.items())))
    print("  logical bytes in regular files: %d (%.2f GB)"
          % (bytes_logical, bytes_logical / 1e9))
    print("  decmpfs compression types in use: %s"
          % (", ".join("type %d: %d files" % (k, n) for k, n in sorted(cmp_types.items()))
             or "none"))
    print("  superblock says: files %d, dirs %d, symlinks %d, other %d"
          % (v.num_files, v.num_directories, v.num_symlinks, v.num_other))
    print("  blocks: %d checksums verified, %d bad, %d cache hits"
          % (c.blocks.cksum_checked, c.blocks.cksum_bad, c.blocks.cache_hits))
    c.close()


def main(argv):
    want = None
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "-v" and i + 1 < len(argv):
            want = argv[i + 1]
            i += 2
            continue
        args.append(argv[i])
        i += 1
    if not args:
        print(__doc__ or "", file=sys.stderr)
        print("usage: apfs.py {info|ls|tree|cat|stat|census} IMAGE [PATH] [-v VOLUME]",
              file=sys.stderr)
        return 2
    cmd = args[0]
    try:
        if cmd == "info":
            cmd_info(args[1], want)
        elif cmd == "ls":
            cmd_ls(args[1], args[2] if len(args) > 2 else "/", want)
        elif cmd == "tree":
            cmd_tree(args[1], args[2] if len(args) > 2 else "/", want)
        elif cmd == "cat":
            cmd_cat(args[1], args[2], want)
        elif cmd == "stat":
            cmd_stat(args[1], args[2], want)
        elif cmd == "census":
            cmd_census(args[1], want)
        else:
            raise ApfsError("unknown command %r" % cmd)
    except ApfsError as e:
        print("apfs.py: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
