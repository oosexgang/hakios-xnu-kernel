#!/usr/bin/env python3
"""Build an APFS container from directories: the port's own read-only volumes, as images.

usage: mkapfs.py -o IMAGE [-z [--plain DIR]...] [--time SECONDS] [--slack BLOCKS] NAME=DIR[,manifest=FILE] [NAME=DIR ...]

Each NAME=DIR becomes one volume of the container (volume 0 first), case-sensitive and
normalization-insensitive like the iOS volumes, unencrypted, read-only content laid out once.
Permission bits and symbolic links are taken from DIR; everything is owned by root:wheel
unless a manifest says otherwise (lines of: mode in octal with the type bits, uid, gid, path,
[link target]; ios165/system.manifest is one; a directory or symbolic link that is only in the
manifest is made). With -z a regular file is stored compressed when that makes it smaller,
as Apple stores most of the iOS root volume: a small one whole in its decmpfs attribute (type
3, no block of its own at all), a bigger one in 64 KB zlib pieces in its resource fork (type 4); but not what is under a --plain DIR (relative to the volume's root), which stays as
it is and in one piece -- the firmware a driver uses where it lies in memory.  Times are all --time (default: 2023-05-12, the iOS 16.5 images'), so the same
input gives the same image byte for byte.

The image is what Apple's newfs_apfs and a single transaction would leave (one checkpoint,
transaction 1): the container superblock, the checkpoint (its map, the space manager with
its internal pool, the reaper, the two free queues), the container's object map, and per
volume its superblock, object map, file-system tree, extent-reference tree and an empty
snapshot tree; then the files' data.  What the layout of the space manager has to be is
taken from linux-apfs/apfsprogs' mkapfs (and checked with its apfsck, which has to find the
image clean: tools/apfs/check-image.sh).  Hard links are not made: a name is a file.
"""
import argparse
import concurrent.futures
import hashlib
import os
import stat
import struct
import sys
import unicodedata
import zlib

BS = 4096
XID = 1
OID_NX = 1
OID_RESERVED = 1024                     # the first object id that is not reserved
ROOT_DIR_PARENT, ROOT_DIR_INO, PRIV_DIR_INO = 1, 2, 3
MIN_USER_INO = 16

# object types
T_NX, T_BTREE, T_BTNODE, T_SPACEMAN, T_CAB, T_CIB = 0x1, 0x2, 0x3, 0x5, 0x6, 0x7
T_FREE_QUEUE, T_OMAP, T_CPMAP, T_FS, T_FSTREE, T_BLOCKREF, T_SNAPMETA, T_REAPER = 0x9, 0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x11
VIRTUAL, EPHEMERAL, PHYSICAL = 0x0, 0x80000000, 0x40000000

# b-tree node flags, tree flags
BTNODE_ROOT, BTNODE_LEAF, BTNODE_FIXED = 0x1, 0x2, 0x4
BTREE_ALLOW_GHOSTS, BTREE_EPHEMERAL, BTREE_PHYSICAL, BTREE_KV_NONALIGNED = 0x4, 0x8, 0x10, 0x40
BTOFF_INVALID = 0xffff

# file-system records
J_SNAP_METADATA, J_EXTENT, J_INODE, J_XATTR, J_SIBLING_LINK, J_DSTREAM_ID = 1, 2, 3, 4, 5, 6
J_FILE_EXTENT, J_DIR_REC = 8, 9
DT_DIR, DT_REG, DT_LNK = 4, 8, 10
INODE_HAS_RSRC_FORK, INODE_NO_RSRC_FORK, INODE_HAS_UNCOMPRESSED_SIZE = 0x4000, 0x8000, 0x40000
UF_COMPRESSED = 0x20
INO_EXT_TYPE_NAME, INO_EXT_TYPE_DSTREAM = 4, 8
XF_DO_NOT_COPY, XF_SYSTEM_FIELD = 0x2, 0x20
XATTR_DATA_STREAM, XATTR_DATA_EMBEDDED, XATTR_FS_OWNED = 0x1, 0x2, 0x4
KIND_NEW = 1

DECMPFS_MAGIC = 0x636d7066              # 'cmpf'
DECMPFS_ZLIB_ATTR, DECMPFS_ZLIB_RSRC = 3, 4
XATTR_MAX_EMBEDDED = 3804               # the most an attribute's value can hold in its record
PIECE = 65536

SM_BITMAP_XID_OFF = 0x150               # (apfsprogs' mkapfs: from a test image)
IP_BM_TX_MULTIPLIER = 16
IP_BM_INDEX_INVALID = 0xffff


def fletcher64(block):
    """The checksum of an object: Fletcher-64 over its 32-bit words after the first eight bytes."""
    words = struct.unpack_from('<%dI' % ((len(block) - 8) // 4), block, 8)
    s1 = s2 = 0
    for w in words:
        s1 = (s1 + w) % 0xffffffff
        s2 = (s2 + s1) % 0xffffffff
    c1 = 0xffffffff - ((s1 + s2) % 0xffffffff)
    c2 = 0xffffffff - ((s1 + c1) % 0xffffffff)
    return (c2 << 32) | c1


def seal(obj, oid, otype, subtype=0):
    """Fill in an object's header (oid, transaction, type) and its checksum, in place."""
    struct.pack_into('<QQII', obj, 8, oid, XID, otype, subtype)
    struct.pack_into('<Q', obj, 0, fletcher64(obj))


_CRC32C = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    _CRC32C.append(_c)


def name_hash(name):
    """A directory record's hash: CRC-32C (no final inversion) over the name decomposed, as UTF-32."""
    h = 0xffffffff
    for ch in unicodedata.normalize('NFD', name):
        for b in struct.pack('<I', ord(ch)):
            h = (h >> 8) ^ _CRC32C[(h ^ b) & 0xff]
    return h & 0x3fffff


def div_up(a, b):
    return (a + b - 1) // b


# ---------------------------------------------------------------------- the input tree

class Node:
    def __init__(self, name, mode, source=None, target=None):
        self.name = name                # str
        self.mode = mode                # st_mode
        self.uid = self.gid = 0
        self.source = source            # a regular file's path on this machine
        self.target = target            # a symbolic link's target, bytes
        self.children = {}              # name -> Node, for a directory
        self.oid = 0


def read_tree(path, name=''):
    st = os.lstat(path)
    if stat.S_ISDIR(st.st_mode):
        node = Node(name, st.st_mode)
        for entry in sorted(os.listdir(path)):
            child = read_tree(os.path.join(path, entry), entry)
            if child is not None:
                node.children[entry] = child
        return node
    if stat.S_ISREG(st.st_mode):
        return Node(name, st.st_mode, source=path)
    if stat.S_ISLNK(st.st_mode):
        return Node(name, st.st_mode, target=os.fsencode(os.readlink(path)))
    return None                         # device nodes, FIFOs, sockets: /dev is devfs


def apply_manifest(root, manifest):
    for raw in open(manifest, 'rb'):
        fields = raw.rstrip(b'\n').split(b'\t')
        if len(fields) < 4 or not fields[3]:
            continue
        mode, uid, gid = int(fields[0], 8), int(fields[1]), int(fields[2])
        parts = fields[3].decode('utf-8', 'surrogateescape').split('/')
        node = root
        for part in parts[:-1]:
            node = node.children.get(part)
            if node is None or not stat.S_ISDIR(node.mode):
                break
        else:
            leaf = node.children.get(parts[-1])
            if leaf is None:
                if stat.S_ISDIR(mode):
                    leaf = node.children[parts[-1]] = Node(parts[-1], mode)
                elif stat.S_ISLNK(mode) and len(fields) > 4:
                    leaf = node.children[parts[-1]] = Node(parts[-1], mode, target=fields[4])
                else:
                    continue
            # (owner, group and permissions; not the kind: a link this tree has where the manifest
            # says directory -- the system image's LaunchDaemons -- stays a link)
            if stat.S_IFMT(leaf.mode) == stat.S_IFMT(mode):
                leaf.mode, leaf.uid, leaf.gid = mode, uid, gid


# ---------------------------------------------------------------------- b-tree nodes

class Tree:
    """A b-tree laid out from records already in order, leaves first, full nodes."""

    def __init__(self, subtype, storage, fixed=None, tree_flags=0):
        self.subtype = subtype          # T_FSTREE, T_OMAP, ...
        self.storage = storage          # VIRTUAL or PHYSICAL
        self.fixed = fixed              # (key_size, val_size) for a tree of fixed sizes
        self.tree_flags = tree_flags
        self.levels = []                # [[(records, ...)]] per level, leaves first

    @staticmethod
    def entry_size(fixed, klen, vlen):
        return (4 if fixed else 8) + klen + vlen

    def plan(self, records):
        """records: [(key bytes, value bytes)] in order.  Splits them into nodes (no addresses yet)."""
        self.records = records
        self.levels = []
        level = self._pack(records, leaf=True)
        self.levels.append(level)
        while len(level) > 1:
            index = [(node[0][0], b'\0' * 8) for node in level]   # child pointers come later
            level = self._pack(index, leaf=False)
            self.levels.append(level)
        return sum(len(lv) for lv in self.levels)

    def toc_len(self, n, leaf):
        """The table of contents for n entries: a node of a fixed-size tree has its whole table up
        front (as newfs_apfs and mkapfs make it), any other a multiple of 8 entries."""
        if self.fixed is not None:
            # (index nodes too, their values being 8-byte child pointers -- apfsck insists)
            ks, vs = self.fixed
            return ((BS - 56) // (ks + (vs if leaf else 8) + 4)) * 4
        entry = 4 if self.fixed is not None else 8
        return div_up(max(n, 8), 8) * 8 * entry

    def _pack(self, records, leaf):
        nodes, cur, kv = [], [], 0
        for k, v in records:
            # every node is sized as though it were the root, with its 40-byte btree_info
            if cur and 56 + self.toc_len(len(cur) + 1, leaf) + kv + len(k) + len(v) > BS - 40:
                nodes.append(cur)
                cur, kv = [], 0
            cur.append((k, v))
            kv += len(k) + len(v)
        nodes.append(cur)
        return nodes

    def node_count(self):
        return sum(len(lv) for lv in self.levels)

    def build(self, addrs, oids):
        """Make the nodes.  addrs: block address of every node, root first (top level down);
        oids: their object ids (= addresses for a physical tree).  Returns {address: block}."""
        out = {}
        flat = []
        for lvl in range(len(self.levels) - 1, -1, -1):
            for i in range(len(self.levels[lvl])):
                flat.append((lvl, i))
        where = {key: (addrs[n], oids[n]) for n, key in enumerate(flat)}
        longest_k = max((len(k) for k, v in self.records), default=0)
        longest_v = max((len(v) for k, v in self.records), default=0)
        for (lvl, i), (addr, oid) in where.items():
            recs = self.levels[lvl][i]
            if lvl > 0:
                # the child pointers: the children's ids (virtual) or addresses (physical)
                first_child = sum(len(n) for n in self.levels[lvl - 1][:0])
                children_before = sum(len(self.levels[lvl][j]) for j in range(i))
                recs = [(k, struct.pack('<Q', where[(lvl - 1, children_before + c)][1]))
                        for c, (k, _) in enumerate(recs)]
            is_root = (lvl == len(self.levels) - 1)
            out[addr] = self._node(recs, lvl, is_root, oid, longest_k, longest_v)
        return out

    def _node(self, recs, level, is_root, oid, longest_k, longest_v):
        block = bytearray(BS)
        fixed = self.fixed is not None
        leaf = level == 0
        flags = (BTNODE_ROOT if is_root else 0) | (BTNODE_LEAF if leaf else 0) | (BTNODE_FIXED if fixed else 0)
        val_end = BS - (40 if is_root else 0)
        toc_len = self.toc_len(len(recs), leaf)
        key_base = 56 + toc_len
        koff = 0
        voff = 0
        for n, (k, v) in enumerate(recs):
            block[key_base + koff:key_base + koff + len(k)] = k
            voff += len(v)
            block[val_end - voff:val_end - voff + len(v)] = v
            if fixed:
                struct.pack_into('<HH', block, 56 + 4 * n, koff, voff)
            else:
                struct.pack_into('<HHHH', block, 56 + 8 * n, koff, len(k), voff, len(v))
            koff += len(k)
        free = val_end - voff - (key_base + koff)
        if free < 0:
            raise RuntimeError('a b-tree node overflows')
        struct.pack_into('<HHI', block, 32, flags, level, len(recs))
        struct.pack_into('<HHHHHHHH', block, 40, 0, toc_len, koff, free, BTOFF_INVALID, 0, BTOFF_INVALID, 0)
        if is_root:
            ks, vs = self.fixed if fixed else (0, 0)
            lk, lv = (ks, vs) if fixed else (longest_k, longest_v)
            struct.pack_into('<IIIIIIQQ', block, BS - 40, self.tree_flags, BS, ks, vs, lk, lv,
                             len(self.records), self.node_count())
        otype = (T_BTREE if is_root else T_BTNODE) | self.storage
        seal(block, oid, otype, self.subtype)
        return bytes(block)


# ---------------------------------------------------------------------- volumes

def jkey(oid, jtype):
    return struct.pack('<Q', oid | (jtype << 60))


def xfields(fields):
    """An inode's (or a record's) extended fields: [(type, flags, data bytes)] in that order."""
    head = struct.pack('<HH', len(fields), sum(div_up(len(d), 8) * 8 for _, _, d in fields))
    descs = b''.join(struct.pack('<BBH', t, f, len(d)) for t, f, d in fields)
    data = b''.join(d + b'\0' * (div_up(len(d), 8) * 8 - len(d)) for _, _, d in fields)
    return head + descs + data


class Volume:
    def __init__(self, index, name, root, compress, when, spool=None, plain=()):
        self.spool = spool              # where compressed resource forks wait to be written (a file)
        self.plain = tuple(d.strip('/') + '/' for d in plain)   # never compressed below these
        self.index = index
        self.name = name
        self.root = root
        self.compress = compress
        self.when = when
        self.streams = []               # (owning private id, bytes, is_rsrc) -- data to place
        self.counts = {'files': 0, 'dirs': 0, 'symlinks': 0}
        self.next_oid = MIN_USER_INO

    def new_oid(self):
        oid = self.next_oid
        self.next_oid += 1
        return oid

    def collect(self):
        """Inode numbers, contents (compressed or not) and the records that do not depend on
        where the data lands; the extents are added once it is known (place_data)."""
        self.items = []                 # (node, parent oid)
        self.root.oid = ROOT_DIR_INO

        def walk(node, parent, path):
            for name in sorted(node.children):
                child = node.children[name]
                child.oid = self.new_oid()
                child.path = path + name
                self.items.append((child, node.oid))
                if stat.S_ISDIR(child.mode):
                    walk(child, child.oid, child.path + '/')
        walk(self.root, ROOT_DIR_INO, '')
        # private id -> where its bytes are: ('file', path, size) or ('spool', offset, size); never in memory
        self.data = {}
        self.decmpfs = {}               # file oid -> (header, rsrc stream id)
        self.inline = {}                # file oid -> its decmpfs attribute: header and the zlib stream
        files = [node for node, parent in self.items if stat.S_ISREG(node.mode)]
        for node in files:
            node.size = os.path.getsize(node.source)
        forks = iter(())
        if self.compress:
            # compressed by as many processes as there are processors, taken in the files' order
            pool = concurrent.futures.ProcessPoolExecutor()
            forks = pool.map(compressed_fork, [n.source for n in files if n.size > 0 and not self.is_plain(n)],
                             chunksize=16)
        for node in files:
            if node.size == 0:
                continue
            if self.compress and not self.is_plain(node):
                kind, rsrc = next(forks)
                if kind == 'inline':
                    self.inline[node.oid] = struct.pack('<IIQ', DECMPFS_MAGIC, DECMPFS_ZLIB_ATTR, node.size) + rsrc
                    continue
                if kind == 'rsrc':
                    sid = self.new_oid()
                    self.spool.seek(0, 2)
                    self.data[sid] = ('spool', self.spool.tell(), len(rsrc))
                    self.spool.write(rsrc)
                    self.decmpfs[node.oid] = (struct.pack('<IIQ', DECMPFS_MAGIC, DECMPFS_ZLIB_RSRC, node.size), sid)
                    continue
            self.data[node.oid] = ('file', node.source, node.size)
        if self.compress:
            pool.shutdown()

    def is_plain(self, node):
        return any(node.path.startswith(p) for p in self.plain)

    def place(self, first_block):
        """Data in order of private id, each stream in one extent.  Returns the next free block."""
        self.extents = {}
        at = first_block
        for sid in sorted(self.data):
            n = div_up(self.data[sid][2], BS)
            self.extents[sid] = (at, n)
            at += n
        self.data_blocks = at - first_block
        return at

    def fs_records(self):
        """Every record of the file-system tree, in key order."""
        recs = []
        self.counts = {'files': 0, 'dirs': 0, 'symlinks': 0}
        t = self.when
        children = {ROOT_DIR_INO: 0}
        for node, parent in self.items:
            children[parent] = children.get(parent, 0) + 1

        def inode(oid, parent, node_mode, uid, gid, name, nchild_or_link, internal, bsd_flags=0,
                  dstream=None, uncompressed=0):
            fields = [(INO_EXT_TYPE_NAME, XF_DO_NOT_COPY, name.encode('utf-8', 'surrogateescape') + b'\0')]
            if dstream is not None:
                fields.append((INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD, dstream))
            val = struct.pack('<QQQQQQQiIIIIIHHQ', parent, oid, t, t, t, t, internal, nchild_or_link, 0, 0,
                              bsd_flags, uid, gid, node_mode, 0, uncompressed) + xfields(fields)
            return (jkey(oid, J_INODE), val)

        def dstream(size, blocks):
            return struct.pack('<QQQQQ', size, blocks * BS, 0, 0, 0)

        def drec(parent, name, oid, dtype):
            enc = name.encode('utf-8', 'surrogateescape') + b'\0'
            key = jkey(parent, J_DIR_REC) + struct.pack('<I', (name_hash(name) << 10) | len(enc)) + enc
            return (key, struct.pack('<QQH', oid, t, dtype))

        def xattr(oid, name, flags, data):
            enc = name.encode() + b'\0'
            return (jkey(oid, J_XATTR) + struct.pack('<H', len(enc)) + enc,
                    struct.pack('<HH', flags, len(data)) + data)

        def extents(sid, own_stream=True):
            """A stream's extent, and for a file's own stream its reference count; an attribute's
            stream has none (apfsck: \"xattrs can't be cloned\")."""
            start, n = self.extents[sid]
            recs = [(jkey(sid, J_FILE_EXTENT) + struct.pack('<Q', 0), struct.pack('<QQQ', n * BS, start, 0))]
            if own_stream:
                recs.insert(0, (jkey(sid, J_DSTREAM_ID), struct.pack('<I', 1)))
            return recs

        by_oid = {}
        # the two special directories, named by records of the root's parent
        by_oid.setdefault(ROOT_DIR_PARENT, []).extend([
            drec(ROOT_DIR_PARENT, 'private-dir', PRIV_DIR_INO, DT_DIR),
            drec(ROOT_DIR_PARENT, 'root', ROOT_DIR_INO, DT_DIR)])
        r = self.root
        by_oid.setdefault(ROOT_DIR_INO, []).append(
            inode(ROOT_DIR_INO, ROOT_DIR_PARENT, r.mode, r.uid, r.gid, 'root', children.get(ROOT_DIR_INO, 0),
                  INODE_NO_RSRC_FORK))
        by_oid.setdefault(PRIV_DIR_INO, []).append(
            inode(PRIV_DIR_INO, ROOT_DIR_PARENT, stat.S_IFDIR | 0o755, 0, 0, 'private-dir', 0, INODE_NO_RSRC_FORK))
        for node, parent in self.items:
            oid = node.oid
            mine = by_oid.setdefault(oid, [])
            if stat.S_ISDIR(node.mode):
                self.counts['dirs'] += 1
                mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, children.get(oid, 0),
                                  INODE_NO_RSRC_FORK))
                dtype = DT_DIR
            elif stat.S_ISLNK(node.mode):
                self.counts['symlinks'] += 1
                mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, 1, INODE_NO_RSRC_FORK))
                mine.append(xattr(oid, 'com.apple.fs.symlink', XATTR_DATA_EMBEDDED | XATTR_FS_OWNED,
                                  node.target + b'\0'))
                dtype = DT_LNK
            else:
                self.counts['files'] += 1
                if oid in self.inline:
                    mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, 1,
                                      INODE_NO_RSRC_FORK | INODE_HAS_UNCOMPRESSED_SIZE, UF_COMPRESSED,
                                      uncompressed=node.size))
                    mine.append(xattr(oid, 'com.apple.decmpfs', XATTR_DATA_EMBEDDED, self.inline[oid]))
                elif oid in self.decmpfs:
                    header, sid = self.decmpfs[oid]
                    rsrc_len = self.data[sid][2]
                    mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, 1,
                                      INODE_HAS_RSRC_FORK | INODE_HAS_UNCOMPRESSED_SIZE, UF_COMPRESSED,
                                      uncompressed=node.size))
                    xd = struct.pack('<Q', sid) + dstream(rsrc_len, self.extents[sid][1])
                    mine.append(xattr(oid, 'com.apple.ResourceFork', XATTR_DATA_STREAM, xd))
                    mine.append(xattr(oid, 'com.apple.decmpfs', XATTR_DATA_EMBEDDED, header))
                    by_oid.setdefault(sid, []).extend(extents(sid, own_stream=False))
                elif oid in self.data:
                    mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, 1, INODE_NO_RSRC_FORK,
                                      dstream=dstream(node.size, self.extents[oid][1])))
                    mine.extend(extents(oid))
                else:
                    mine.append(inode(oid, parent, node.mode, node.uid, node.gid, node.name, 1, INODE_NO_RSRC_FORK))
                dtype = DT_REG
            by_oid.setdefault(parent, []).append(drec(parent, node.name, oid, dtype))
        for oid in sorted(by_oid):
            recs.extend(sorted(by_oid[oid], key=self.sort_key))
        return recs

    @staticmethod
    def sort_key(rec):
        k = rec[0]
        jtype = struct.unpack_from('<Q', k)[0] >> 60
        if jtype == J_DIR_REC:
            return (jtype, struct.unpack_from('<I', k, 8)[0] & ~0x3ff, k[12:])
        if jtype == J_XATTR:
            return (jtype, 0, k[10:])
        if jtype == J_FILE_EXTENT:
            return (jtype, struct.unpack_from('<Q', k, 8)[0], b'')
        return (jtype, 0, b'')

    def extentref_records(self):
        recs = []
        for sid in sorted(self.extents, key=lambda s: self.extents[s][0]):
            start, n = self.extents[sid]
            recs.append((jkey(start, J_EXTENT), struct.pack('<QQI', n | (KIND_NEW << 60), sid, 1)))
        return recs


def compressed_fork(path):
    """How a file is best kept: ('inline', its zlib stream) when that fits the decmpfs attribute,
    ('rsrc', its resource fork) when that takes fewer blocks, else (None, None)."""
    content = open(path, 'rb').read()
    if len(content) <= PIECE:
        z = zlib.compress(content, 9)
        if 16 + len(z) <= XATTR_MAX_EMBEDDED:
            return 'inline', z
    rsrc = zlib_rsrc(content)
    return ('rsrc', rsrc) if div_up(len(rsrc), BS) < div_up(len(content), BS) else (None, None)


def zlib_rsrc(content):
    """A resource fork as decmpfs keeps zlib pieces in it (type 4): an HFS resource fork whose one
    resource is a table of the 64 KB pieces, each zlib (or stored, after a 0xff)."""
    pieces = []
    for at in range(0, len(content), PIECE):
        chunk = content[at:at + PIECE]
        z = zlib.compress(chunk, 9)
        pieces.append(z if len(z) < len(chunk) else b'\xff' + chunk)
    count = len(pieces)
    table_len = 4 + 8 * count
    body = bytearray(struct.pack('<I', count))
    off = table_len
    for p in pieces:
        body += struct.pack('<II', off, len(p))
        off += len(p)
    for p in pieces:
        body += p
    data_off = 0x100
    data = struct.pack('>I', len(body)) + bytes(body)
    # the resource map: Apple's own for these forks (the 'cmpf' resource, id 1, named nothing)
    rmap = bytearray(50)
    rmap_off = data_off + len(data)
    struct.pack_into('>IIII', rmap, 0, data_off, rmap_off, len(data), 50)
    struct.pack_into('>HH', rmap, 24, 28, 50)               # type list at 28, name list at 50 (none)
    struct.pack_into('>H', rmap, 28, 0)                     # one type (count - 1)
    rmap[30:34] = b'cmpf'
    struct.pack_into('>HH', rmap, 34, 0, 10)                # one resource of it, its list at 10 from the type list
    struct.pack_into('>hHI', rmap, 38, 1, 0xffff, 0)        # id 1, no name, data at 0 (attrs 0)
    head = struct.pack('>IIII', data_off, rmap_off, len(data), 50)
    out = head + b'\0' * (data_off - len(head)) + data + bytes(rmap)
    return out


# ---------------------------------------------------------------------- the container

def cpoint_desc_blocks(block_count):
    if block_count < 512 * 1024 // 4:
        return 8
    if block_count < 1024 * 1024 // 4:
        return 12
    if block_count < 50 * 1024 * 1024 // 4:
        return 20 + 60 * ((block_count - 1024 * 1024 // 4) // (512 * 1024 // 4)) // 23
    return 280


def cpoint_data_blocks(block_count):
    b = block_count
    if b < 4545:
        return 52
    if b < 13633:
        return 124
    if b < 36353:
        return 160 + 36 * ((b - 13633) // 4544)
    if b < 131777:
        return 308 + 4 * ((b - 36353) // 4544)
    if b < 262144:
        return 648 + 4 * ((b - 131777) // 4544)
    if b == 262144:
        return 992
    if b < 1048576:
        off = (b - 262144) // 131072
        return 1248 + 488 * off + 4 * ((b - (261280 + off * 131776)) // 2272)
    if b < 4063232:
        return 4112 + 256 * ((b - 1048576) // 131072)
    if b < 13107200:
        return 10000 + 256 * ((b - 4063232) // 131072)
    return 27672


def main_fq_node_limit(blocks):
    if blocks < 0x40000:
        r = 1 + (blocks - 1) // 4544
    elif blocks < 0x100000:
        r = 116 + (blocks - 261281) // 2272
    else:
        r = 512
    return 3 if r == 2 else r


def ip_fq_node_limit(chunks):
    r = 3 * (chunks + 751) // 1127 - 1
    return 3 if r == 2 else r


class Container:
    def __init__(self, volumes, slack, uuid_seed):
        self.vols = volumes
        self.slack = slack
        self.seed = uuid_seed

    def uuid(self, what):
        return hashlib.sha256((self.seed + what).encode()).digest()[:16]

    def layout(self):
        """Decide every block: done twice, since the container's size decides the sizes of the
        checkpoint areas and of the space manager, which come before everything else."""
        for v in self.vols:
            v.collect()
        guess = 1024
        while True:
            total = self._layout(guess)
            if total + self.slack <= guess:
                break
            guess = total + self.slack
        self.block_count = guess

    def _layout(self, block_count):
        self.bc = block_count
        bpc = BS * 8                                    # blocks per chunk: one bitmap block each
        self.chunks = div_up(block_count, bpc)
        self.chunks_per_cib = (BS - 40) // 32
        self.cibs = div_up(self.chunks, self.chunks_per_cib)
        self.cibs_per_cab = (BS - 40) // 8
        cabs = div_up(self.cibs, self.cibs_per_cab)
        self.cabs = 0 if cabs == 1 else cabs
        self.ip_blocks = (self.chunks + self.cibs + self.cabs) * 3
        self.ip_bm_size = div_up(self.ip_blocks, bpc)
        self.ip_bmap_blocks = 16 * self.ip_bm_size
        self.bm_addr_off = SM_BITMAP_XID_OFF + 8 * self.ip_bm_size
        self.bm_free_next_off = self.bm_addr_off + div_up(2 * self.ip_bm_size, 8) * 8
        self.cib_addr_off = self.bm_free_next_off + self.ip_bmap_blocks * 2
        entries = self.cabs if self.cabs > 1 else self.cibs
        self.sm_size = div_up(entries * 8 + self.cib_addr_off, BS) * BS
        self.sm_blocks = self.sm_size // BS

        self.desc_base = 1
        self.desc_blocks = cpoint_desc_blocks(block_count)
        self.data_base = self.desc_base + self.desc_blocks
        self.data_blocks = cpoint_data_blocks(block_count)
        self.reaper_bno = self.data_base
        self.sm_bno = self.reaper_bno + 1
        self.ipfq_bno = self.sm_bno + self.sm_blocks
        self.mainfq_bno = self.ipfq_bno + 1
        self.eph_blocks = self.mainfq_bno - self.reaper_bno + 1
        at = self.data_base + self.data_blocks
        self.nx_omap_bno, self.nx_omap_root = at, at + 1
        at += 2
        # object ids: the ephemeral objects, then each volume's superblock and tree nodes
        oid = OID_RESERVED
        self.sm_oid, self.reaper_oid, self.ipfq_oid, self.mainfq_oid = oid, oid + 1, oid + 2, oid + 3
        oid += 4
        for v in self.vols:
            v.oid = oid
            oid += 1
            v.sb_bno = at
            v.omap_bno = at + 1
            at += 2
            # the trees: their shapes do not depend on the data's addresses (fixed-size fields)
            v.place(0)
            v.fs_tree = Tree(T_FSTREE, VIRTUAL, tree_flags=BTREE_KV_NONALIGNED)
            n_fs = v.fs_tree.plan(v.fs_records())
            v.fs_oids = list(range(oid, oid + n_fs))
            oid += n_fs
            v.omap_tree = Tree(T_OMAP, PHYSICAL, fixed=(16, 16), tree_flags=BTREE_PHYSICAL)
            n_om = v.omap_tree.plan([(struct.pack('<QQ', o, XID), b'\0' * 16) for o in v.fs_oids])
            v.omap_addrs = list(range(at, at + n_om))
            at += n_om
            v.fs_addrs = list(range(at, at + n_fs))
            at += n_fs
            v.er_tree = Tree(T_BLOCKREF, PHYSICAL, tree_flags=BTREE_PHYSICAL | BTREE_KV_NONALIGNED)
            n_er = v.er_tree.plan(v.extentref_records())
            v.er_addrs = list(range(at, at + n_er))
            at += n_er
            v.snap_bno = at
            at += 1
        self.next_oid = oid
        self.ip_bm_base = at
        at += self.ip_bmap_blocks
        self.ip_base = at
        at += self.ip_blocks
        for v in self.vols:
            at = v.place(at)
            # the extents are known now: the records again, with them (same sizes, same shape)
            v.fs_tree.plan(v.fs_records())
            v.er_tree.plan(v.extentref_records())
        self.used_end = at
        return at

    def write(self, path):
        bc = self.bc
        blocks = {}
        used = bytearray(div_up(bc, 8))

        def mark(start, n=1):
            b, end = start, start + n
            while b < end and b & 7:
                used[b >> 3] |= 1 << (b & 7)
                b += 1
            if end - b >= 8:
                used[b >> 3:(end >> 3)] = b'\xff' * ((end >> 3) - (b >> 3))
                b = end & ~7
            while b < end:
                used[b >> 3] |= 1 << (b & 7)
                b += 1

        def put(bno, data):
            assert len(data) % BS == 0
            blocks[bno] = data

        bpc = BS * 8
        used_chunks = div_up(self.used_end, bpc)
        chunk_bmap_base = self.ip_base
        cib_base = chunk_bmap_base + used_chunks
        cab_base = cib_base + self.cibs

        # --- per volume: trees, object map, superblock
        for v in self.vols:
            fs_blocks = v.fs_tree.build(v.fs_addrs, v.fs_oids)
            for b, d in fs_blocks.items():
                put(b, d)
            omap_recs = [(struct.pack('<QQ', o, XID), struct.pack('<IIQ', 0, BS, a))
                         for o, a in zip(v.fs_oids, v.fs_addrs)]
            v.omap_tree.plan(omap_recs)
            for b, d in v.omap_tree.build(v.omap_addrs, v.omap_addrs).items():
                put(b, d)
            omap = bytearray(BS)
            struct.pack_into('<IIIIQ', omap, 32, 0, 0, PHYSICAL | T_BTREE, PHYSICAL | T_BTREE, v.omap_addrs[0])
            seal(omap, v.omap_bno, PHYSICAL | T_OMAP)
            put(v.omap_bno, bytes(omap))
            for b, d in v.er_tree.build(v.er_addrs, v.er_addrs).items():
                put(b, d)
            snap = Tree(T_SNAPMETA, PHYSICAL, tree_flags=BTREE_PHYSICAL | BTREE_KV_NONALIGNED)
            snap.plan([])
            for b, d in snap.build([v.snap_bno], [v.snap_bno]).items():
                put(b, d)
            alloc = 1 + len(v.omap_addrs) + len(v.fs_addrs) + len(v.er_addrs) + 1 + v.data_blocks
            sb = bytearray(BS)
            struct.pack_into('<4sI', sb, 32, b'APSB', v.index)
            struct.pack_into('<QQQ', sb, 40, 0x2, 0, 0x8)           # hard-link map records; normalization-insensitive
            struct.pack_into('<Q', sb, 0x58, alloc)
            struct.pack_into('<HHIIIHH', sb, 0x60, 5, 0, 0, 6, 0, 1, 0)   # meta crypto: v5.0, class F, revision 1
            struct.pack_into('<III', sb, 0x74, VIRTUAL | T_BTREE, PHYSICAL | T_BTREE, PHYSICAL | T_BTREE)
            struct.pack_into('<QQQQ', sb, 0x80, v.omap_bno, v.fs_oids[0], v.er_addrs[0], v.snap_bno)
            struct.pack_into('<Q', sb, 0xb0, v.next_oid)
            struct.pack_into('<QQQQQ', sb, 0xb8, v.counts['files'], v.counts['dirs'], v.counts['symlinks'], 0, 0)
            struct.pack_into('<QQ', sb, 0xe0, alloc, 0)
            sb[0xf0:0x100] = self.uuid('volume %d' % v.index)
            struct.pack_into('<QQ', sb, 0x100, v.when, 1)          # last modified; unencrypted
            fmt = b'mkapfs.py (darwin-redwood)'
            sb[0x110:0x110 + len(fmt)] = fmt
            struct.pack_into('<QQ', sb, 0x130, v.when, XID)
            nm = v.name.encode()[:255]
            sb[0x2c0:0x2c0 + len(nm)] = nm
            struct.pack_into('<I', sb, 0x3c0, 3)                   # next document id
            seal(sb, v.oid, VIRTUAL | T_FS)
            put(v.sb_bno, bytes(sb))
            for b in [v.sb_bno, v.omap_bno] + v.omap_addrs + v.fs_addrs + v.er_addrs + [v.snap_bno]:
                mark(b)
            for start, n in v.extents.values():
                mark(start, n)

        # --- the container's object map: the volumes' superblocks
        nx_omap = Tree(T_OMAP, PHYSICAL, fixed=(16, 16), tree_flags=BTREE_PHYSICAL)
        nx_omap.plan([(struct.pack('<QQ', v.oid, XID), struct.pack('<IIQ', 0, BS, v.sb_bno)) for v in self.vols])
        assert nx_omap.node_count() == 1
        for b, d in nx_omap.build([self.nx_omap_root], [self.nx_omap_root]).items():
            put(b, d)
        omap = bytearray(BS)
        struct.pack_into('<IIIIQ', omap, 32, 1, 0, PHYSICAL | T_BTREE, PHYSICAL | T_BTREE, self.nx_omap_root)
        seal(omap, self.nx_omap_bno, PHYSICAL | T_OMAP)
        put(self.nx_omap_bno, bytes(omap))

        # --- what else is in use: block 0, the checkpoint areas, the internal pool and its bitmaps
        mark(0)
        mark(self.desc_base, self.desc_blocks)
        mark(self.data_base, self.data_blocks)
        mark(self.nx_omap_bno, 2)
        mark(self.ip_bm_base, self.ip_bmap_blocks)
        mark(self.ip_base, self.ip_blocks)

        # --- the space manager: chunk bitmaps, chunk-info blocks, the internal pool's own bitmap
        free_total = 0
        cib_blocks = []
        chunk = 0
        for c in range(self.cibs):
            cib = bytearray(BS)
            n = min(self.chunks_per_cib, self.chunks - chunk)
            struct.pack_into('<II', cib, 32, c, n)
            for i in range(n):
                start = chunk * bpc
                count = min(bpc, bc - start)
                bm = bytes(used[start // 8:(start + count + 7) // 8])
                in_use = sum(bin(x).count('1') for x in bm)
                bmap_addr = 0
                if chunk < used_chunks:
                    bmap_addr = chunk_bmap_base + chunk
                    put(bmap_addr, bm + b'\0' * (BS - len(bm)))
                struct.pack_into('<QQIIQ', cib, 40 + 32 * i, XID, start, count, count - in_use, bmap_addr)
                free_total += count - in_use
                chunk += 1
            seal(cib, cib_base + c, PHYSICAL | T_CIB)
            cib_blocks.append(bytes(cib))
            put(cib_base + c, bytes(cib))
        for c in range(self.cabs):
            cab = bytearray(BS)
            n = min(self.cibs_per_cab, self.cibs - c * self.cibs_per_cab)
            struct.pack_into('<II', cab, 32, c, n)
            for i in range(n):
                struct.pack_into('<Q', cab, 40 + 8 * i, cib_base + c * self.cibs_per_cab + i)
            seal(cab, cab_base + c, PHYSICAL | T_CAB)
            put(cab_base + c, bytes(cab))
        ip_bm = bytearray(self.ip_bm_size * BS)
        for b in list(range(chunk_bmap_base, chunk_bmap_base + used_chunks)) + \
                list(range(cib_base, cib_base + self.cibs)) + list(range(cab_base, cab_base + self.cabs)):
            r = b - self.ip_base
            ip_bm[r >> 3] |= 1 << (r & 7)
        put(self.ip_bm_base, bytes(ip_bm))
        for i in range(self.ip_bm_size, self.ip_bmap_blocks):
            put(self.ip_bm_base + i, bytes(BS))

        sm = bytearray(self.sm_size)
        struct.pack_into('<IIII', sm, 32, BS, bpc, self.chunks_per_cib, self.cibs_per_cab)
        struct.pack_into('<QQIIQIIQ', sm, 48, bc, self.chunks, self.cibs, self.cabs, free_total,
                         self.cib_addr_off, 0, 0)
        struct.pack_into('<IIQIIQQQQ', sm, 144, 0, IP_BM_TX_MULTIPLIER, self.ip_blocks, self.ip_bm_size,
                         self.ip_bmap_blocks, self.ip_bm_base, self.ip_base, 0, 0)
        struct.pack_into('<QQQHHIQ', sm, 200, 0, self.ipfq_oid, 0, ip_fq_node_limit(self.chunks), 0, 0, 0)
        struct.pack_into('<QQQHHIQ', sm, 240, 0, self.mainfq_oid, 0, main_fq_node_limit(bc), 0, 0, 0)
        struct.pack_into('<HHIII', sm, 320, self.ip_bm_size, self.ip_bmap_blocks - 1, SM_BITMAP_XID_OFF,
                         self.bm_addr_off, self.bm_free_next_off)
        for i in range(self.ip_bm_size):
            struct.pack_into('<Q', sm, SM_BITMAP_XID_OFF + 8 * i, XID)
            struct.pack_into('<H', sm, self.bm_addr_off + 2 * i, i)
        for i in range(self.ip_bmap_blocks):
            if i < self.ip_bm_size or i == self.ip_bmap_blocks - 1:
                nxt = IP_BM_INDEX_INVALID
            else:
                nxt = i + 1
            struct.pack_into('<H', sm, self.bm_free_next_off + 2 * i, nxt)
        if self.cabs:
            for i in range(self.cabs):
                struct.pack_into('<Q', sm, self.cib_addr_off + 8 * i, cab_base + i)
        else:
            for i in range(self.cibs):
                struct.pack_into('<Q', sm, self.cib_addr_off + 8 * i, cib_base + i)
        seal(sm, self.sm_oid, EPHEMERAL | T_SPACEMAN)
        put(self.sm_bno, bytes(sm))

        # --- the reaper and the two free queues (empty)
        reaper = bytearray(BS)
        struct.pack_into('<Q', reaper, 32, 1)
        struct.pack_into('<I', reaper, 64, 1)                       # nr_flags: BHM
        struct.pack_into('<I', reaper, 108, BS - 112)               # nr_state_buffer_size
        seal(reaper, self.reaper_oid, EPHEMERAL | T_REAPER)
        put(self.reaper_bno, bytes(reaper))
        for bno, oid in ((self.ipfq_bno, self.ipfq_oid), (self.mainfq_bno, self.mainfq_oid)):
            fq = bytearray(BS)
            toc = ((BS - 56) // (16 + 8 + 4)) * 4
            struct.pack_into('<HHI', fq, 32, BTNODE_ROOT | BTNODE_LEAF | BTNODE_FIXED, 0, 0)
            struct.pack_into('<HHHHHHHH', fq, 40, 0, toc, 0, BS - 56 - toc - 40, BTOFF_INVALID, 0, BTOFF_INVALID, 0)
            struct.pack_into('<IIIIIIQQ', fq, BS - 40, BTREE_EPHEMERAL | BTREE_ALLOW_GHOSTS, BS, 16, 8, 16, 8, 0, 1)
            seal(fq, oid, EPHEMERAL | T_BTREE, T_FREE_QUEUE)
            put(bno, bytes(fq))

        # --- the container superblock, the checkpoint map, the superblock's copy
        nx = bytearray(BS)
        struct.pack_into('<4sIQQQQ', nx, 32, b'NXSB', BS, bc, 0, 0, 0x2)     # incompatible: version 2
        nx[72:88] = self.uuid('container')
        struct.pack_into('<QQ', nx, 88, self.next_oid, XID + 1)
        struct.pack_into('<IIQQIIIIII', nx, 104, self.desc_blocks, self.data_blocks, self.desc_base, self.data_base,
                         2, self.eph_blocks, 0, 2, 0, self.eph_blocks)
        struct.pack_into('<QQQII', nx, 152, self.sm_oid, self.nx_omap_bno, self.reaper_oid, 0,
                         min(100, div_up(bc * BS, 512 * 1024 * 1024)))
        for i, v in enumerate(self.vols):
            struct.pack_into('<Q', nx, 184 + 8 * i, v.oid)
        min_blocks = main_fq_node_limit(bc) if bc * BS < 128 * 1024 * 1024 else 8
        struct.pack_into('<Q', nx, 1312, (min_blocks << 32) | (4 << 16) | 1)
        seal(nx, OID_NX, EPHEMERAL | T_NX)
        put(0, bytes(nx))
        cpm = bytearray(BS)
        maps = [(EPHEMERAL | T_REAPER, 0, BS, self.reaper_oid, self.reaper_bno),
                (EPHEMERAL | T_SPACEMAN, 0, self.sm_size, self.sm_oid, self.sm_bno),
                (EPHEMERAL | T_BTREE, T_FREE_QUEUE, BS, self.ipfq_oid, self.ipfq_bno),
                (EPHEMERAL | T_BTREE, T_FREE_QUEUE, BS, self.mainfq_oid, self.mainfq_bno)]
        struct.pack_into('<II', cpm, 32, 1, len(maps))             # the last map of the checkpoint
        for i, (t, st, size, oid, paddr) in enumerate(maps):
            struct.pack_into('<IIIIQQQ', cpm, 40 + 40 * i, t, st, size, 0, 0, oid, paddr)
        seal(cpm, self.desc_base, PHYSICAL | T_CPMAP)
        put(self.desc_base, bytes(cpm))
        put(self.desc_base + 1, bytes(nx))

        # --- out, in order (holes stay holes in a sparse file); the files' data straight from where it is
        with open(path, 'wb') as f:
            f.truncate(bc * BS)
            for bno in sorted(blocks):
                f.seek(bno * BS)
                f.write(blocks[bno])
            for v in self.vols:
                for sid, (start, n) in sorted(v.extents.items(), key=lambda e: e[1][0]):
                    kind, where, size = v.data[sid]
                    f.seek(start * BS)
                    if kind == 'file':
                        with open(where, 'rb') as src:
                            left = size
                            while left > 0:
                                chunk = src.read(min(left, 1 << 22))
                                if not chunk:
                                    raise RuntimeError('%s is shorter than it was' % where)
                                f.write(chunk)
                                left -= len(chunk)
                    else:
                        v.spool.seek(where)
                        f.write(v.spool.read(size))
        return free_total


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('-z', '--compress', action='store_true', help='zlib in the resource fork where it saves space')
    ap.add_argument('--plain', action='append', default=[], help='with -z: never compress what is under this directory')
    ap.add_argument('--time', type=int, default=1683900000, help='every timestamp, in seconds')
    ap.add_argument('--slack', type=int, default=256, help='free blocks to leave (at least)')
    ap.add_argument('volumes', nargs='+', help='NAME=DIR[,manifest=FILE]')
    args = ap.parse_args()
    vols = []
    spool = open(args.output + '.spool', 'w+b') if args.compress else None
    for i, spec in enumerate(args.volumes):
        name, _, rest = spec.partition('=')
        parts = rest.split(',')
        root = read_tree(parts[0])
        for opt in parts[1:]:
            k, _, val = opt.partition('=')
            if k == 'manifest':
                apply_manifest(root, val)
        vols.append(Volume(i, name, root, args.compress, args.time * 1000000000, spool, args.plain))
    c = Container(vols, args.slack, os.path.basename(args.output))
    c.layout()
    free = c.write(args.output)
    if spool is not None:
        spool.close()
        os.unlink(args.output + '.spool')
    for v in vols:
        print('mkapfs.py: volume %d "%s": %d files, %d directories, %d symbolic links, %d blocks of data%s' % (
            v.index, v.name, v.counts['files'], v.counts['dirs'], v.counts['symlinks'], v.data_blocks,
            ' (%d compressed in their resource fork, %d whole in their attribute)' % (len(v.decmpfs), len(v.inline))
            if v.compress else ''))
    print('mkapfs.py: %s: %d blocks of %d bytes, %d free' % (args.output, c.block_count, BS, free))


if __name__ == '__main__':
    main()
