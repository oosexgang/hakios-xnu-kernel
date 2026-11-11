/*
 * Reading the Apple File System: the part that knows the format and nothing about vnodes.
 *
 * The same file is built into the kernel and, by tools/apfs/build-tool.sh, into a program
 * on the build machine (tools/apfs/apfs-tool.c) that runs it against a real image and
 * compares the result with tools/apfs/apfs.py, which is itself checked against 7-Zip.
 * Nothing goes into the kernel that has not agreed with the image on the computer first.
 *
 * The layout being read is in apfs_format.h, written from Apple's reference.
 *
 * Where the bytes come from is the caller's business (struct apfs_dev): in the kernel a
 * block device, in the tool a raw image.  Every structure is checked before it is used --
 * a block's Fletcher-64, every offset and length inside a B-tree node, every record's
 * length against the fields read out of it -- so a damaged or hostile image comes back as
 * EIO and never as a wild pointer.
 *
 * Read only, and nothing here ever writes to the device.
 *
 * What is read: unencrypted volumes, sealed or not; the live tree only (no snapshots);
 * inodes, directories, symbolic links, hard links, extended attributes (embedded and in
 * their own data stream), file extents and holes, and files the operating system
 * compressed (decmpfs: zlib and stored, in the attribute or in the resource fork).
 * What is not: encryption, snapshots, Fusion, and writing of any kind.
 */

#ifndef _MISCFS_APFS_APFS_LIB_H_
#define _MISCFS_APFS_APFS_LIB_H_

#include "apfs_format.h"

/*
 * A clock for the breakdown counters below.  Raw ticks: the kernel's are mach absolute
 * time and are turned into nanoseconds where they are printed, the host's already are
 * nanoseconds.
 */
#ifdef KERNEL
#include <kern/clock.h>
#define apfs_ticks()    mach_absolute_time()
#else
#include <time.h>
static inline uint64_t
apfs_ticks(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#ifdef KERNEL
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <kern/kalloc.h>
#define apfs_alloc(n)        kalloc_data((vm_size_t)(n), Z_WAITOK | Z_ZERO)
#define apfs_free(p, n)      kfree_data((p), (vm_size_t)(n))
#define apfs_log(...)        printf("apfs: " __VA_ARGS__)
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define apfs_alloc(n)        calloc(1, (n))
#define apfs_free(p, n)      ((void)(n), free(p))
#define apfs_log(...)        fprintf(stderr, "apfs: " __VA_ARGS__)
#endif

/*
 * How many blocks of metadata to keep (8 MB at 4096 bytes a block).  64 was plenty from a
 * disk in memory; from the phone's flash a block not kept is a random 4 KB read of 113 us.
 * A walk of the whole root volume read 123940 blocks with 64 kept, 83841 with 512, 63789
 * with 2048 and 61831 with 8192.  Found through a hash of the address, the one to give up
 * picked by a clock hand (a block used since the hand last passed gets another round).
 */
#define APFS_CACHE_BLOCKS 2048
#define APFS_CACHE_HASH   4096          /* a power of two */

/* Room for a symbolic link's target (Darwin's own limit is 1024 bytes; it is kept in an xattr) */
#define APFS_LINK_MAX 4096

/* How many pieces a sequential reader's read-ahead decompresses at a time */
#define APFS_RA_PIECES 7

/*
 * The device.  read() must fill the whole buffer or return an error; offsets and lengths
 * are always whole blocks of the container.
 *
 * read_data() is the same for the contents of files, which are read once and not kept:
 * straight into the caller's memory, past any cache, and called without the caller's lock
 * -- so it must be safe on several threads at once and touch nothing apfs_lib keeps.
 * NULL: read() is used for those too (still without the lock, so read() must then be safe
 * that way as well).
 *
 * write() is for apfs_write.c and nothing else: whole blocks, and only ever blocks that
 * hold the contents of one file (apfs_write_in_place).  A device that leaves it NULL -- as
 * every mount of this driver does -- cannot be written to at all, which is what it was
 * before this field existed.
 */
struct apfs_dev {
	void     *ctx;
	uint64_t  size;                 /* bytes, 0 if not known */
	int     (*read)(void *ctx, uint64_t off, void *buf, size_t len);
	int     (*read_data)(void *ctx, uint64_t off, void *buf, size_t len);
	int     (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
};

/* A cached block's memory, given up while somebody may still be reading it (nx->retired) */
struct apfs_retired {
	struct apfs_retired *next;
	uint64_t             epoch;
	uint8_t             *data;
};

/* One cached block of metadata. */
struct apfs_buf {
	uint64_t  addr;                 /* block address, APFS_BUF_EMPTY if unused */
	int32_t   next;                 /* the next in its hash chain, or -1 */
	bool      ref;                  /* used since the clock hand last passed it */
	uint32_t  used;                 /* nx->fetches when it was last used */
	bool      checked;              /* its checksum has been verified */
	uint8_t  *data;
};

/*
 * Blocks of metadata are read a few at a time, aligned: the one asked for and its
 * neighbours, which a walk mostly wants next (meta_cluster, boot-arg apfs_meta_cluster,
 * up to APFS_META_CLUSTER).  From the phone's flash, walking the whole root volume: one
 * block a read, 64459 reads and 7294 ms; four, 36086 and 5680 ms; eight, 27918 and
 * 5673 ms; sixteen, 26214 and 6937 ms -- a random read costs about 100 us whatever its
 * size, and then the bytes.  The walk: 46328 / 44701 / 44823 / 45439 ms.
 */
#define APFS_META_CLUSTER         16
#define APFS_META_CLUSTER_DEFAULT 4

/* A block used fewer than this many fetches ago is never given up (see nx_block) */
#define APFS_CACHE_RECENT 64

#define APFS_BUF_EMPTY ((uint64_t)~0ULL)

/* A B-tree, ready to be walked. */
struct apfs_btree {
	struct apfs_nx *nx;
	struct apfs_omap *omap;         /* NULL for a tree whose node ids are block addresses */
	uint64_t  root;                 /* block address of the root node */
	uint64_t  root_oid;             /* its object id: a sealed tree names its nodes from it */
	uint32_t  flags;                /* bt_flags out of the root's btree_info */
	uint32_t  key_size;             /* nonzero only for fixed-size keys */
	uint32_t  val_size;
	uint64_t  key_count;
	uint64_t  node_count;
};

/* An object map: it turns an object identifier into a block address. */
struct apfs_omap {
	struct apfs_btree tree;
	bool              ready;
};

/*
 * What the object maps have answered: a B-tree node of the file-system tree is named by
 * a virtual id, and every step down that tree used to be a walk down an object map too.
 * The mount is read-only at one checkpoint, so an answer never goes stale.  One cache
 * for the container's map and its volumes', direct-mapped, made at apfs_nx_open.
 */
#define APFS_OMAP_CACHE 2048            /* a power of two */

struct apfs_omap_hit {
	uint64_t map;                   /* which object map: the root of its tree */
	uint64_t oid;                   /* 0: empty */
	uint64_t xid;
	uint64_t addr;
};

struct apfs_scratch;            /* apfs_lib.c's own */

/* A mounted container. */
struct apfs_nx {
	struct apfs_dev   dev;
	uint32_t          block_size;
	uint64_t          block_count;
	apfs_xid_t        xid;          /* the checkpoint we are reading */
	uint64_t          omap_addr;
	apfs_oid_t        fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
	uint32_t          fs_count;
	uint8_t           uuid[16];
	struct apfs_omap  omap;         /* resolves volume superblocks */
	/*
	 * The checkpoint rings, as the live superblock names them.  The read path does not
	 * need the data ring at all and reads the descriptor ring only at open; they are kept
	 * so that apfs_write.c can refuse outright a write that would land in either of them
	 * -- the one bound that does not depend on having read any tree correctly.
	 */
	uint64_t          xp_desc_base, xp_data_base;
	uint32_t          xp_desc_blocks, xp_data_blocks;
	/*
	 * The caller's lock round this container, which apfs_lib may let go of while it
	 * does work that touches nothing shared -- reading a file's contents off the device
	 * (dev.read_data), decompressing a piece into a buffer no one else sees -- and take
	 * back afterwards.  Everything else here (the block cache, the cursors, a file's held
	 * piece, the scratch list) is only touched with it held.  NULL for a caller
	 * with no lock, such as the single-threaded program on the build machine.
	 */
	void            (*unlock)(void *ctx);
	void            (*relock)(void *ctx);
	void             *lock_ctx;
	struct apfs_buf  *cache;        /* APFS_CACHE_BLOCKS of them, made at apfs_nx_open */
	int32_t          *cache_hash;   /* APFS_CACHE_HASH chain heads, -1 for none */
	uint32_t          hand;
	uint32_t          fetches;      /* blocks read into the cache, ever (wraps) */
	uint8_t          *cluster;      /* APFS_META_CLUSTER blocks, made at the first miss */
	/*
	 * With a caller's lock (unlock/relock set), a block of metadata missing from the cache is
	 * read with the lock let go of -- other threads go on meanwhile -- into memory of its own,
	 * and put in afterwards (unless somebody else put it in first). So that nobody ever reads
	 * a cached block's memory while it is given to another block, a block that leaves the
	 * cache leaves its memory behind on `retired`, tagged with `epoch`, and the cache slot
	 * gets new memory; the caller frees what no operation still running can be looking at
	 * (apfs_nx_reclaim, with the oldest epoch any of its operations began at).
	 */
	uint64_t          lookups_hashed, lookups_scanned;     /* names found by their hash, by reading the directory */
	uint64_t          epoch;
	struct apfs_retired *retired;   /* newest first */
	uint64_t          retired_now, retired_most, unlocked_fills, fill_races;
	uint32_t          meta_cluster; /* blocks a metadata read brings in, 1..APFS_META_CLUSTER (0: the default) */
	/*
	 * A request to the device costs next to nothing (a disk in memory): no reading pieces
	 * together in batch_group, one block a metadata read.  Set by the caller; from the
	 * phone's memory disk batch_group cost one reader 612 -> 663 ms over the restore
	 * ramdisk, from its flash it saved 52.9 -> 46.3 s over the root volume.
	 */
	bool              cheap_requests;
	uint64_t          prefetched, prefetch_used;   /* neighbours brought in, and later asked for */
	uint64_t          reads;
	uint64_t          hits;
	/*
	 * Where the time goes, to tell apart waiting for the device from decompressing.
	 * Ticks, not nanoseconds -- see apfs_ticks().
	 */
	uint64_t          dev_calls, dev_ticks, dev_bytes;
	/* the same for file contents, read without the lock (read_data) */
	uint64_t          data_calls, data_ticks, data_bytes;
	/*
	 * The highest block either kind of read has reached.  What it is for: the phone has
	 * no driver for its own storage, so a volume there has to live in a RAM disk, and
	 * that says how much of the front of the image would be enough.
	 */
	uint64_t          max_addr;             /* any block */
	uint64_t          max_data_addr;        /* blocks of file contents only */
	uint64_t          inflate_calls, inflate_ticks, inflate_in, inflate_out;
	uint64_t          inflate_fallbacks;   /* streams libdeflate refused and zlib took */
	/*
	 * Decompressors and buffers for reading compressed pieces, kept from one read to the
	 * next instead of allocated for each; touched only with the lock held.
	 */
	struct apfs_scratch *scratch;
	uint32_t          scratch_kept;
	/*
	 * Several threads for one request: run fn(arg, item, worker) for every item below
	 * `items`, on up to `workers` threads at once, the caller's among them, each with a
	 * worker number below `workers`; return the first error any item gave.  Called
	 * without the lock, and fn takes none.  NULL (the program on the build machine): the
	 * items are done one after another on the caller's thread.
	 */
	int             (*parallel)(void *ctx, uint32_t items, uint32_t workers,
	    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg);
	void             *parallel_ctx;
	uint32_t          parallel_max;         /* threads it can bring, the caller's included */
	/*
	 * Reads in flight on the container, kept by the caller with the lock held.  The
	 * threads nx->parallel brings are shared out among them, parallel_max / requests
	 * each: with four, all of them to a request that is alone, two each to two, none
	 * from three up -- the processors are busy already, and on the phone four readers
	 * came out 7% slower with helpers than without.  0 (the program on the build
	 * machine) counts as alone.
	 */
	uint32_t          requests;
	uint64_t          batch_calls, batch_pieces;   /* how many requests used it, for how much */
	uint64_t          ra_batches, ra_pieces, ra_hits;  /* read-ahead: filled, and used */
	struct apfs_omap_hit *omap_cache;       /* APFS_OMAP_CACHE of them, or NULL */
	uint64_t          omap_hits, omap_misses;
};

/* A volume inside the container. */
struct apfs_vol {
	struct apfs_nx   *nx;
	uint32_t          index;
	apfs_oid_t        oid;
	uint64_t          incompat;
	uint64_t          fs_flags;
	uint16_t          role;
	/*
	 * Whether directory-entry keys carry a hash of the name.  Apple's reference
	 * describes both shapes of the key but never says which a volume uses; the hash it
	 * defines (CRC-32C of the name decomposed and widened) is only of use for looking
	 * a name up without regard to case or normalization, and the iOS 16.5 volume, which
	 * is normalization-insensitive, does carry it.  So: hashed when either of those two
	 * features is on.  A wrong guess does not go unnoticed -- the name length would not
	 * fit the key and the record is refused.
	 */
	bool              hashed_names;
	/*
	 * A sealed volume (the iOS 16 cryptexes) keeps its files' extents in a tree of their own,
	 * keyed by stream and offset, rather than among the file-system tree's records.
	 */
	struct apfs_btree fext;
	bool              has_fext;
	/*
	 * The extent-reference tree: which stream owns a run of physical blocks and how many
	 * references there are to it.  The read path has no use for it -- a file's own extent
	 * records say where its bytes are -- but it is the only place that says whether those
	 * blocks are shared with a clone, so apfs_write.c will not write a block without it.
	 * Only the identifier is kept here, and the tree is opened into the caller's own memory
	 * (apfs_extentref_open): a whole struct apfs_btree more in every mount would put
	 * struct apfs_mount over the 16 KB a mount is allowed (apfs_vfsops.c).
	 */
	apfs_oid_t        extentref_oid;        /* a block address: the tree is physical; 0 if none */
	uint64_t          num_snapshots;        /* a snapshot shares blocks with the live tree */
	apfs_oid_t        integrity_meta_oid;   /* a sealed volume: hashes of the files' contents */
	uint64_t          num_files;
	uint64_t          num_dirs;
	uint64_t          num_symlinks;
	uint64_t          alloc_count;
	char              name[APFS_VOLNAME_LEN];
	struct apfs_omap  omap;         /* resolves the nodes of the tree below */
	struct apfs_btree tree;         /* the file-system tree */
};

/* One record out of a file-system tree: pointers into a cached block. */
struct apfs_rec {
	const uint8_t *key;
	uint16_t       key_len;
	const uint8_t *val;
	uint16_t       val_len;
	uint64_t       obj_id;
	uint8_t        type;
};

/*
 * A walk over the records of a tree.  It holds one cached block per level, so a cursor
 * must not outlive the container and only one may be open at a time per container unless
 * the cache is larger than the tree is deep.
 */
#define APFS_MAX_LEVELS 8

struct apfs_cursor {
	struct apfs_btree *tree;
	int                depth;
	uint64_t           node[APFS_MAX_LEVELS];  /* the block address at each level,
	                                            * never an object identifier */
	uint32_t           index[APFS_MAX_LEVELS];
	uint64_t           hash8[APFS_MAX_LEVELS];  /* a sealed tree's nodes: the first 8 bytes of the
	                                            * SHA-256 the parent keeps of each (0 for the root) */
	bool               done;
};

/* What an inode record says, after the extended fields have been picked apart. */
struct apfs_inode {
	uint64_t obj_id;
	uint64_t parent_id;
	uint64_t private_id;            /* the data stream's id, for a regular file */
	uint64_t create_time;
	uint64_t mod_time;
	uint64_t change_time;
	uint64_t access_time;
	uint64_t internal_flags;
	int32_t  nchildren;             /* a directory: children; otherwise: links */
	uint32_t bsd_flags;
	uint32_t owner;
	uint32_t group;
	uint16_t mode;
	uint64_t size;                  /* the logical size, what stat reports */
	uint64_t alloced_size;
	uint32_t rdev;
	bool     has_dstream;
	bool     is_sparse;
};

/* One extent of a data stream. */
struct apfs_extent {
	uint64_t logical;
	uint64_t length;
	uint64_t phys;                  /* 0 is a hole */
};

/* An extended attribute, as found in its record. */
struct apfs_xattr {
	uint16_t       flags;
	const uint8_t *data;            /* embedded value, or an apfs_j_xattr_dstream */
	uint16_t       data_len;
	uint64_t       stream_id;       /* when the value is in its own stream */
	uint64_t       stream_size;
};

/* No piece is being held. */
#define APFS_NO_PIECE ((uint64_t)~0ULL)

/*
 * How a file's contents are stored, and what has been worked out about it so far.
 *
 * A compressed file's bytes are cut into 64 KB pieces, each its own zlib stream, with a
 * table in the resource fork saying where each piece is.  Reaching that table costs
 * several walks of the file-system tree -- the attribute record, the fork's header, the
 * table entry -- and a piece has to be decompressed whole however few of its bytes are
 * wanted.  A reader that comes back for the next page would pay all of that again, which
 * is what made paging a mapped file slow.
 *
 * So this holds what has been found: the fork's stream, the whole table, and the last
 * piece decompressed.  One of these belongs to whoever is reading the file -- the driver
 * keeps one per vnode -- and it is handed to apfs_read_file() on every call.
 * apfs_cfile_fini() gives back what it holds.  Passing NULL instead works and is correct,
 * and is the slow way.
 */
struct apfs_cfile {
	bool      ready;                /* set up, for the inode in obj_id */
	uint64_t  obj_id;
	bool      compressed;
	uint32_t  type;                 /* decmpfs compression type */
	uint64_t  size;                 /* uncompressed size */
	bool      in_rsrc;              /* the bytes are in com.apple.ResourceFork */

	/* the resource fork and its table of pieces, read once */
	uint64_t  rsrc_id;
	uint64_t  rsrc_size;
	uint64_t  table_base;
	uint32_t  piece_count;
	uint32_t *piece_off;            /* piece_count entries, from table_base */
	uint32_t *piece_len;

	/* the piece last decompressed */
	uint64_t  held;                 /* its index, or APFS_NO_PIECE */
	uint8_t  *held_data;
	size_t    held_len;
	size_t    held_cap;             /* 0 when this file is not worth holding */

	/*
	 * Read-ahead.  A reader with threads to spare (alone on the volume, or one of two)
	 * that asks for the piece right after the one its last read ended on is taken to be
	 * reading the file through: the pieces
	 * after it are decompressed in the same batch, on the other threads, into ra_data,
	 * and its next reads are copies out of that.  All of it happens inside the request
	 * that asked; ra_filling keeps everyone else off ra_data while the lock is let go of.
	 */
	uint64_t  ra_expect;            /* the piece the last whole-piece read ended before */
	uint64_t  ra_first;             /* the first piece in ra_data */
	uint32_t  ra_count;             /* how many are there; 0: none */
	bool      ra_filling;
	uint8_t  *ra_data;              /* APFS_RA_PIECES pieces, made when first wanted */
	uint32_t  ra_len[APFS_RA_PIECES];
};

/* ---- container and volumes ---- */

int apfs_nx_open(struct apfs_nx *nx, const struct apfs_dev *dev);
void apfs_nx_close(struct apfs_nx *nx);
/* The hash APFS keeps with a name in its directory record (22 bits); false: the name cannot be hashed */
bool apfs_name_hash(const char *name, size_t name_len, bool fold, uint32_t *hash);
/* Free the retired blocks' memory no operation that began at `oldest` or later can see (lock held) */
void apfs_nx_reclaim(struct apfs_nx *nx, uint64_t oldest);
#ifndef KERNEL
/* For tests on the build machine: every cached block out (retired, with a caller's lock), so all is read again */
void apfs_nx_cache_flush(struct apfs_nx *nx);
/* Every directory record of the volume, with the hash stored in its key (hashed keys only) */
int apfs_dirrec_walk(struct apfs_vol *vol, int (*cb)(void *ctx, uint64_t dir_id, const char *name,
    size_t name_len, uint32_t stored_hash, uint64_t file_id), void *ctx);
#endif
int apfs_vol_open(struct apfs_nx *nx, uint32_t index, struct apfs_vol *vol);

/* ---- records ---- */

/*
 * Put a cursor on the first record of (obj_id, type), or on the first record after it if
 * there is none.  A file-system tree sorts by object id first and record type second, so
 * the records of one object and type are a run; apfs_cursor_next() walks it and the
 * caller stops when obj_id or type changes.
 */
int apfs_seek(struct apfs_btree *tree, uint64_t obj_id, uint8_t type,
    struct apfs_cursor *cur);
int apfs_cursor_next(struct apfs_cursor *cur, struct apfs_rec *rec);

/* ---- what the records mean ---- */

/*
 * Read an inode record.  ino->size is the size of the data stream, which for a compressed
 * file or a symbolic link is not the size a reader of the file sees -- those keep their
 * contents elsewhere.  This stays cheap, touching only the one record; use
 * apfs_inode_size() where the size reported to a caller has to be right.
 */
int apfs_inode_get(struct apfs_vol *vol, uint64_t obj_id, struct apfs_inode *ino);

/*
 * The logical size of a file: the uncompressed size for a compressed one, the length of
 * the target for a symbolic link, the data stream's size otherwise.  Costs an attribute
 * lookup for anything that is not a plain file.
 */
int apfs_inode_size(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *size);
int apfs_lookup(struct apfs_vol *vol, uint64_t dir_id, const char *name, size_t name_len,
    uint64_t *file_id, uint8_t *d_type);
int apfs_path_lookup(struct apfs_vol *vol, const char *path, uint64_t *file_id,
    uint8_t *d_type);

typedef int (*apfs_dir_cb)(void *ctx, const char *name, size_t name_len,
    uint64_t file_id, uint8_t d_type);
int apfs_readdir(struct apfs_vol *vol, uint64_t dir_id, apfs_dir_cb cb, void *ctx);

/*
 * Find an attribute and report where its value is kept.  Only the fields that are values
 * are filled in -- xa->data is always NULL, because a record's bytes live in a cached
 * block that the next read may reuse.  To get at the value, use apfs_xattr_read().
 */
int apfs_xattr_lookup(struct apfs_vol *vol, uint64_t obj_id, const char *name,
    struct apfs_xattr *xa);

/*
 * The value of one attribute by name, wherever it is kept: copied straight out of the
 * record while it is still in hand if it is embedded there, read from its data stream if
 * it has one.  *out is how many bytes were put in buf, which is less than the whole value
 * if buf is too small.
 */
int apfs_xattr_read(struct apfs_vol *vol, uint64_t obj_id, const char *name, void *buf,
    size_t len, size_t *out);

/*
 * Every attribute of an object.  Inside the callback xa->data points at the value if it
 * is embedded, and is good only until the callback returns; a callback that wants to keep
 * it copies it.  Returning nonzero from the callback ends the walk with that value.
 */
typedef int (*apfs_xattr_cb)(void *ctx, const char *name, size_t name_len,
    const struct apfs_xattr *xa);
int apfs_xattr_list(struct apfs_vol *vol, uint64_t obj_id, apfs_xattr_cb cb, void *ctx);

/* The target of a symbolic link, without the trailing zero byte. */
int apfs_readlink(struct apfs_vol *vol, uint64_t obj_id, char *buf, size_t len,
    size_t *out);

/* ---- file contents ---- */

/*
 * Raw bytes of a data stream, holes reading as zeros.  `stream_id` is an inode's
 * private_id for a file, or an attribute's stream id.  Short reads only at end of stream.
 */
int apfs_read_stream(struct apfs_vol *vol, uint64_t stream_id, uint64_t stream_size,
    uint64_t off, void *buf, size_t len, size_t *out);

/*
 * The extent of a data stream that covers logical offset `off`: where it begins, how long
 * it is, and the block it starts at (0 for a hole).  ENOENT past the last extent.  A
 * reader of contents has no use for this -- apfs_read_stream walks the extents itself --
 * but a writer has to know exactly which blocks are the file's own.
 */
int apfs_extent_at(struct apfs_vol *vol, uint64_t stream_id, uint64_t off,
    struct apfs_extent *ext);

/*
 * Open the volume's extent-reference tree into `tree`, which is the caller's to keep for as
 * long as the volume is open.  ENOENT when the volume has none.
 */
int apfs_extentref_open(struct apfs_vol *vol, struct apfs_btree *tree);

/*
 * Work out how a file's contents are stored and read in whatever is worth reading once.
 * Calling it again for the same inode does nothing; for a different one it starts over.
 */
int apfs_cfile_init(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf);

/* Give back everything a context holds.  Safe on a zeroed or already finished one. */
void apfs_cfile_fini(struct apfs_cfile *cf);

/*
 * The contents of a file as a reader of the file would see them: the data stream for a
 * plain file, the decompressed bytes for a compressed one.
 */
int apfs_read_file(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf, uint64_t off, void *buf, size_t len, size_t *out);

/*
 * Where a file's bytes lie on the device, when they lie in one piece as they are: not
 * compressed, and one extent from its start covering its size.  *addr is that extent's first
 * block.  ENOENT otherwise (compressed, in pieces, empty).
 */
int apfs_file_in_place(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *addr);

/* ---- odds and ends worth testing on their own ---- */

uint64_t apfs_fletcher64(const uint8_t *data, size_t len);
bool apfs_checksum_ok(const uint8_t *block, uint32_t block_size);
uint16_t apfs_mode_to_dtype(uint16_t mode);

#endif /* _MISCFS_APFS_APFS_LIB_H_ */
