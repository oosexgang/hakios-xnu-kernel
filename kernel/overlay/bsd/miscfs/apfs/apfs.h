/*
 * apfs: reading an Apple File System volume from the kernel.
 *
 * The format itself is in apfs_lib.{h,c}, which knows nothing of vnodes and is checked
 * against real images on the build machine (tools/apfs/run-tests.sh).  This is the layer
 * between that and VFS: a mount, a vnode per object, and the vnode operations.
 *
 * Read only.  Nothing here writes to the device.
 *
 * apfs_lib keeps a cache of metadata blocks and hands out pointers into it, and its
 * cursors are plain structures with no locking of their own, so every call into it is
 * made with am_lock held.  That makes the whole volume one lock wide, which is enough for
 * a read-only root: the work under the lock is a few block reads, usually from the cache.
 */

#ifndef _MISCFS_APFS_APFS_H_
#define _MISCFS_APFS_APFS_H_

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <kern/locks.h>

#include "apfs_lib.h"

/* Buckets in the table that finds an existing vnode by object identifier. */
#define APFS_HASH_BUCKETS 1024

struct apfs_node;

/*
 * What one round of vnop_read reads.  Alone on the volume, am_iobuf_size (boot-arg
 * apfs_round): a read(2) of that much is one request to apfs_lib, whose whole compressed
 * pieces are decompressed on several processors -- the bigger the round, the more there is
 * to share out.  With other reads in flight, APFS_ROUND_SHARED: the processors are busy
 * anyway, and a round that stays in the cache until uiomove copies it out is worth more.
 * One reader with 1 MB reads over the restore ramdisk, rounds of 128 KB / 256 KB / 512 KB /
 * 1 MB: 867 / 663 / 612 / 561 ms; four readers: 1429 / 1534 / 1747 / 2009 ms, and 1327
 * with rounds of 64 or 128 KB and no helpers.
 */
#define APFS_IOBUF_DEFAULT (1024 * 1024)
#define APFS_IOBUF_MAX     (2 * 1024 * 1024)
#define APFS_ROUND_SHARED  (128 * 1024)
#define APFS_IOBUF_KEEP    8

#define APFS_SYNTH_MAX     64
#define APFS_SYNTH_NAMELEN 96
/*
 * Object ids from here up are synthetic: the last few of the 60 bits an id has.  (Not
 * 0x0FFFFFFF00000000: the iOS root volume numbers its own objects from there.)
 */
#define APFS_SYNTH_BASE    0x0FFFFFFFFFFFF000ULL

struct apfs_synth {
	uint64_t parent;                /* the directory it is in (a real one, or synthetic) */
	uint64_t id;                    /* its own, or the directory it is another name for */
	bool     alias;                 /* another name for a directory of the volume */
	char     name[APFS_SYNTH_NAMELEN];
};

struct apfs_mount {
	mount_t            am_mp;
	vnode_t            am_devvp;
	lck_mtx_t          am_lock;
	struct apfs_nx     am_nx;
	struct apfs_vol    am_vol;
	uint32_t           am_devblock;
	bool               am_owns_devvp;  /* we opened it, so we close it */
	bool               am_shares_devvp; /* another container of a device one of ours has open: not opened again */
	dev_t              am_rdev;         /* the device, for those other containers to find (apfs_mounts) */
	struct apfs_mount *am_next;         /* in apfs_mounts */
	struct apfs_node  *am_hash[APFS_HASH_BUCKETS];
	uint64_t           am_root_id;
	/*
	 * Where the time goes at the vnode operations, to set beside the container's own
	 * counters (waiting for the device, decompressing).  Ticks; see apfs_ticks().
	 */
	uint64_t           am_pagein_calls, am_pagein_ticks, am_pagein_bytes;
	/* read(2) through the page cache: bytes copied from pages already there, bytes read into it (atomic) */
	uint64_t           am_read_cached, am_read_filled;
	uint64_t           am_read_calls, am_read_ticks, am_read_bytes;
	/*
	 * What the one lock round apfs_lib costs: how long callers stood waiting for it, and
	 * how long it was held.  Held time is the part of the work that cannot happen on two
	 * processors at once, so with several readers it is the ceiling.
	 */
	uint64_t           am_lock_waits;
	uint64_t           am_lock_wait_ticks;
	uint64_t           am_lock_held_ticks;
	uint64_t           am_lock_t0;      /* when the holder took it; only the holder writes */
	/* how long takes waited: up to 2 us, 10 us, 100 us, 1 ms, more; and the longest (the holder writes) */
	uint64_t           am_lock_wait_hist[5];
	uint64_t           am_lock_wait_max;
	/*
	 * The operations going on (taken apfs_lock, not yet apfs_unlock; apfs_lib may let go of
	 * the lock in between), and the container's epoch each began at: retired cache memory
	 * older than the oldest of them is freed (apfs_nx_reclaim). Under am_lock.
	 */
#define APFS_OPS_MAX 128
	struct {
		thread_t           th;
		uint64_t           epoch;
		const char        *site;        /* the function that began it */
	}                  am_ops[APFS_OPS_MAX];
	/*
	 * The lock by who takes it (the function that called apfs_lock; apfs_lib's taking it back in
	 * the middle of an operation counts for the operation's): takes, waiting (in all, the longest,
	 * how many over 1 ms), holding (the same). Under am_lock.
	 */
#define APFS_LOCK_SITES 32
	struct apfs_lock_site {
		const char        *name;
		uint64_t           takes, relocks;
		uint64_t           wait, wait_max, wait_long;
		uint64_t           hold, hold_max, hold_long;
	}                  am_sites[APFS_LOCK_SITES];
	const char        *am_lock_site;    /* the holder's */
	uint32_t           am_lock_waiting; /* threads waiting for it now (atomic) */
	uint64_t           am_waiters_hist[5];  /* others waiting when it was taken: 0, 1, 2-3, 4-7, 8 or more */
	uint64_t           am_lookup_negative;  /* names not there, now known to the name cache */
	uint64_t           am_lookup_negative_hits;     /* lookups the name cache answered with "not there" */
	uint64_t           am_inactive_free;    /* vnop_inactive with nothing to give back: no lock taken */
	uint64_t           am_lookup_positive_hits;     /* lookups the name cache answered with the vnode */
	uint64_t           am_node_found, am_node_made; /* apfs_node_vnode: a vnode there already, a new one */
	uint64_t           am_node_named;       /* of those there already, entered in the name cache by a lookup */
	/* waiting by the waiter's base priority: up to 4, 20, 31, 46, higher (all waits; those over 1 ms) */
	uint64_t           am_prio_wait[5], am_prio_long[5], am_prio_long_wait[5];
	/*
	 * The waits over 1 ms, by the same priorities, taken apart: blocked (the lock not given),
	 * queued (runnable, no processor to run on), on a processor (spinning, the lock's own work).
	 */
	uint64_t           am_long_blocked[5], am_long_queued[5], am_long_oncpu[5];
	uint64_t           am_long_cpu[8];      /* the processor each one ended on */
	/*
	 * Who waits (waits over 10 us) and who reads (read(2), pagein), by process name; the
	 * first APFS_LOCK_PROCS-1 names, the last entry takes the rest.  Changed under am_lock,
	 * entries only ever added; read without it.  Allocated with the mount (the mount itself
	 * has to stay within kalloc_type's 16 KB); NULL if that failed: then not kept.
	 */
#define APFS_LOCK_PROCS 64
	struct apfs_lock_proc {
		char       name[MAXCOMLEN + 1];
		uint64_t   waits, wait, wait_max, wait_long;
		uint64_t   read_calls, read_bytes, pagein_calls, pagein_bytes;
		uint64_t   read_ticks, pagein_ticks;     /* time in them, added without the lock (atomic) */
	}                 *am_procs;
	uint32_t           am_nprocs;
	lck_spin_t         am_procs_spin;   /* finding or adding an entry (a read from the page cache has no am_lock) */
	bool               am_ubc_read;     /* read(2) through the page cache (boot-arg apfs_ubc_read, default 1) */
	uint32_t           am_nops;
	uint32_t           am_untracked;    /* operations the table had no room for: nothing is freed meanwhile */
	/* how long it was held: up to 10 us, 100 us, 1 ms, 10 ms, more; and the longest */
	uint64_t           am_lock_hold_hist[5];
	uint64_t           am_lock_hold_max;
	/*
	 * vnop_read's buffers, kept for the next read rather than allocated and freed for
	 * every one (at that size the kernel goes to the VM).  Under am_lock.
	 */
	void              *am_iobuf[APFS_IOBUF_KEEP];
	uint32_t           am_iobuf_kept;
	lck_spin_t         am_iobuf_spin;   /* the two above: their own lock, not am_lock */
	uint32_t           am_iobuf_size;
	/*
	 * Synthetic directories: empty directories that are not on the volume, for mount
	 * points on a volume nobody can add a directory to -- what macOS's synthetic.conf is
	 * for its sealed system volume.  Added through sysctl kern.apfs_synthetic; seen by
	 * lookup and readdir of their parent, empty, owned by root.  Or another name for a
	 * directory of the same volume ("PATH=TARGET"): the directory itself, with PATH's
	 * parent for its "..".  Under am_lock.
	 */
	struct apfs_synth *am_synth;        /* APFS_SYNTH_MAX of them, made at the first */
	uint32_t           am_nsynth;
};

/* The private part of a vnode: which object it is. */
struct apfs_node {
	struct apfs_node  *an_next;     /* the hash chain */
	struct apfs_mount *an_mount;
	vnode_t            an_vp;
	uint64_t           an_id;       /* the object identifier */
	uint64_t           an_parent;
	uint16_t           an_mode;
	uint64_t           an_size;     /* the logical size, as a reader of the file sees it */
	bool               an_attached; /* an_vp is set and the node is in the hash */
	/*
	 * What has been worked out about how this file's contents are stored, kept for as
	 * long as the vnode lives: the table of compressed pieces and the piece last
	 * decompressed.  Without it every read and every page fault would find them again.
	 * Guarded by am_lock, like everything else that reaches apfs_lib.
	 */
	struct apfs_cfile  an_cf;
	/*
	 * The inode record as it was when the vnode was made.  Nothing on this volume can
	 * change -- the mount is read only and there is no writer anywhere -- so there is
	 * nothing to invalidate, and a read no longer walks the tree for the record and the
	 * decmpfs header on every call.
	 */
	struct apfs_inode  an_ino;
	/*
	 * A symbolic link's target, read once: every path through the link asks for it again
	 * (/var, /tmp, /etc -- thousands of times a second after a boot), and it cannot change.
	 * Put in once (atomically), read without the lock, freed with the vnode.
	 */
	struct apfs_link  *an_link;
	/* read(2) through the page cache: where a reader in order would read next, and how much it was given ahead (hints, atomic) */
	uint64_t           an_read_next;
	uint32_t           an_read_window;
};

struct apfs_link {
	size_t             len;
	char               text[];
};

#define APFS_MP(mp)       ((struct apfs_mount *)vfs_fsprivate(mp))
const struct apfs_synth *apfs_synth_find(struct apfs_mount *am, uint64_t parent,
    const char *name, size_t len);      /* with am_lock held */

#define APFS_VP(vp)       ((struct apfs_node *)vnode_fsnode(vp))
#define APFS_VP_MOUNT(vp) (APFS_VP(vp)->an_mount)

/* apfs_vfsops.c */
extern struct vfsops apfs_vfsops;
int apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx);
int apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp);
void apfs_node_detach(struct apfs_node *an);

/* the breakdown of where the time went, straight to the log (apfs_vfsops.c) */
void apfs_stats_print(void);

void apfs_lock_at(struct apfs_mount *am, const char *site);  /* an operation begins */
#define apfs_lock(am) apfs_lock_at((am), __func__)
void apfs_unlock(struct apfs_mount *am);         /* and ends */
void apfs_lock_inner(struct apfs_mount *am);     /* the lock taken back in the middle of one (apfs_lib) */
void apfs_unlock_inner(struct apfs_mount *am);   /* let go of in the middle of one */

/* apfs_vnops.c */
extern const struct vnodeopv_desc apfs_vnodeop_opv_desc;
extern int (**apfs_vnodeop_p)(void *);

#endif /* _MISCFS_APFS_APFS_H_ */
