/*
 * apfs mount-level operations.  See apfs.h; the format is in apfs_lib.c.
 */

#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <sys/attr.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <sys/sysctl.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <sys/redwood_input.h>
#include <sys/redwood_virtio_blk.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

static LCK_GRP_DECLARE(apfs_lck_grp, "apfs");

/* Blocks are read from the device in pieces of this size. */
#define APFS_META_CHUNK 4096

/*
 * The device, as apfs_lib wants it.  Offsets and lengths are whole container blocks, and
 * a container block is a multiple of the device's block size (both are powers of two and
 * the container's is at least 4096), so every read lines up.
 */
struct apfs_devctx {
	vnode_t  dc_devvp;
	uint32_t dc_devblock;
	uint64_t dc_base;               /* where on the device the container starts (bytes) */
};

static int
apfs_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		buf_t bp = NULL;
		size_t chunk = len < APFS_META_CHUNK ? len : APFS_META_CHUNK;
		int error = (int)buf_meta_bread(dc->dc_devvp,
		    (daddr64_t)(off / dc->dc_devblock), APFS_META_CHUNK, NOCRED, &bp);

		if (error) {
			if (bp != NULL) {
				buf_brelse(bp);
			}
			return error;
		}
		memcpy(out, (const void *)buf_dataptr(bp), chunk);
		/*
		 * apfs_lib keeps its own cache of what it needs, and a volume this size would
		 * otherwise fill the buffer cache with blocks nobody asks for twice.
		 */
		buf_markinvalid(bp);
		buf_brelse(bp);

		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}

/*
 * A file's contents, straight into the caller's memory: one buffer header of our own and
 * one VNOP_STRATEGY a request, past the buffer cache.  apfs_lib calls this without the
 * volume's lock, from several threads at once; nothing here is shared but the device.
 * (Tried once before, in QEMU, in place of the block cache for the middle of a request and
 * with the lock held, it was slower there; here it is the device reads coming out from
 * under the lock, on a device where one costs a memory copy -- see apfs_lib.c.)
 */
#define APFS_DATA_MAX (128 * 1024)

static int
apfs_dev_read_data(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0 || (len % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < APFS_DATA_MAX ? len : APFS_DATA_MAX;
		buf_t bp = buf_alloc(dc->dc_devvp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, B_READ);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)out);
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		/* read() must fill the whole buffer, and so must this (md cuts the count short) */
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

/* ------------------------------------------------------------- where the time goes */

/*
 * One mounted volume, for the sysctl below to look at.  This port mounts one APFS volume
 * and the counters are a bring-up aid, so a single pointer is enough; it is set when the
 * volume is mounted and cleared when it goes away.
 */
static struct apfs_mount *apfs_the_mount;
static LCK_MTX_DECLARE(apfs_pool_lock, &apfs_lck_grp);   /* the pool's threads, and apfs_mounts */

/*
 * Every APFS mount, for a mount of another container of the same device to find that the
 * device is open already (apfs_mount): the root's RAM disk holds the boot volume and the OS
 * cryptex, each its own container.  Under apfs_pool_lock.
 */
static struct apfs_mount *apfs_mounts;

static bool
apfs_device_held(dev_t rdev)
{
	struct apfs_mount *m;
	bool held = false;

	lck_mtx_lock(&apfs_pool_lock);
	for (m = apfs_mounts; m != NULL && !held; m = m->am_next) {
		held = (m->am_rdev == rdev);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return held;
}

/*
 * Which volume kern.apfs_stats is about: the root's, when the root is APFS (the iOS volume on
 * the flash, once init has switched to it); else the one mounted last.
 */
extern vnode_t rootvnode;

static struct apfs_mount *
apfs_stats_mount(void)
{
	mount_t mp = rootvnode != NULLVP ? vnode_mount(rootvnode) : NULL;

	if (mp != NULL && strcmp(vfs_statfs(mp)->f_fstypename, "apfs") == 0 && vfs_fsprivate(mp) != NULL) {
		return vfs_fsprivate(mp);
	}
	return apfs_the_mount;
}

static uint64_t
apfs_ticks_ms(uint64_t ticks)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(ticks, &ns);
	return ns / 1000000;
}

/* The volume's lock by who takes it, and how many others waited, appended to buf */
static void
apfs_lock_report(struct apfs_mount *am, char *buf, size_t len)
{
	size_t at = strlen(buf);
	uint32_t i;

	at += scnprintf(buf + at, len - at, "lookups  %llu of names not there, now in the name cache as such; %llu "
	    "answered by it so\n", am->am_lookup_negative, am->am_lookup_negative_hits);
	at += scnprintf(buf + at, len - at, "inactive %llu with nothing to give back, without the lock\n",
	    am->am_inactive_free);
	at += scnprintf(buf + at, len - at, "vnodes   %llu found there already (%llu of them by a lookup, its name now in the "
	    "name cache), %llu made; %llu lookups the name cache answered with the vnode\n", am->am_node_found,
	    am->am_node_named, am->am_node_made, am->am_lookup_positive_hits);
	at += scnprintf(buf + at, len - at, "lock     waited by priority (all ms; over 1 ms: how many, ms): background (<=4) %llu; "
	    "%llu, %llu; utility (<=20) %llu; %llu, %llu; default (<=31) %llu; %llu, %llu; user-initiated (<=46) %llu; %llu, "
	    "%llu; interactive %llu; %llu, %llu\n",
	    apfs_ticks_ms(am->am_prio_wait[0]), am->am_prio_long[0], apfs_ticks_ms(am->am_prio_long_wait[0]),
	    apfs_ticks_ms(am->am_prio_wait[1]), am->am_prio_long[1], apfs_ticks_ms(am->am_prio_long_wait[1]),
	    apfs_ticks_ms(am->am_prio_wait[2]), am->am_prio_long[2], apfs_ticks_ms(am->am_prio_long_wait[2]),
	    apfs_ticks_ms(am->am_prio_wait[3]), am->am_prio_long[3], apfs_ticks_ms(am->am_prio_long_wait[3]),
	    apfs_ticks_ms(am->am_prio_wait[4]), am->am_prio_long[4], apfs_ticks_ms(am->am_prio_long_wait[4]));
	{
		static const char *const band[5] = { "background", "utility", "default", "user-initiated", "interactive" };

		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, where the time went (ms: blocked, the lock not "
		    "given / queued, runnable without a processor / on a processor):");
		for (i = 0; i < 5; i++) {
			at += scnprintf(buf + at, len - at, " %s %llu/%llu/%llu%s", band[i],
			    (unsigned long long)apfs_ticks_ms(am->am_long_blocked[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_queued[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_oncpu[i]), i < 4 ? ";" : "\n");
		}
		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, the processor each ended on:");
		for (i = 0; i < 8; i++) {
			at += scnprintf(buf + at, len - at, " cpu %u %llu%s", i, am->am_long_cpu[i], i < 7 ? "," : "\n");
		}
	}
	at += scnprintf(buf + at, len - at, "lock     takes with others waiting: none %llu, 1 %llu, 2-3 %llu, 4-7 %llu, "
	    "8 or more %llu\n", am->am_waiters_hist[0], am->am_waiters_hist[1], am->am_waiters_hist[2],
	    am->am_waiters_hist[3], am->am_waiters_hist[4]);
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		at += scnprintf(buf + at, len - at, "lock     holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to "
		    "10 ms, %llu longer; longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1],
		    am->am_lock_hold_hist[2], am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
	}
	for (i = 0; i < APFS_LOCK_SITES && am->am_sites[i].name != NULL && at < len; i++) {
		const struct apfs_lock_site *ls = &am->am_sites[i];
		uint64_t wmax, hmax;

		absolutetime_to_nanoseconds(ls->wait_max, &wmax);
		absolutetime_to_nanoseconds(ls->hold_max, &hmax);
		at += scnprintf(buf + at, len - at, "lock by  %-22s %7llu takes %7llu again; waited %6llu ms (longest %6llu us, "
		    "%5llu over 1 ms); held %6llu ms (longest %6llu us, %5llu over 1 ms)\n", ls->name, ls->takes, ls->relocks,
		    (unsigned long long)apfs_ticks_ms(ls->wait), wmax / 1000, ls->wait_long,
		    (unsigned long long)apfs_ticks_ms(ls->hold), hmax / 1000, ls->hold_long);
	}
	for (i = 0; am->am_procs != NULL && i < APFS_LOCK_PROCS && i < am->am_nprocs && at < len; i++) {
		const struct apfs_lock_proc *lp = &am->am_procs[i];
		uint64_t wmax;

		/* (only who waited 1 ms or more in all, or read 1 MB or more) */
		if (apfs_ticks_ms(lp->wait) == 0 && lp->read_bytes + lp->pagein_bytes < (1 << 20)) {
			continue;
		}
		absolutetime_to_nanoseconds(lp->wait_max, &wmax);
		at += scnprintf(buf + at, len - at, "who      %-16.16s waited %6llu times over 10 us, %6llu ms (longest %7llu us, "
		    "%4llu over 1 ms); read(2) %6llu calls %10llu bytes %6llu ms; pagein %6llu calls %10llu bytes %6llu ms\n",
		    lp->name, lp->waits, (unsigned long long)apfs_ticks_ms(lp->wait), wmax / 1000, lp->wait_long,
		    lp->read_calls, lp->read_bytes, (unsigned long long)apfs_ticks_ms(lp->read_ticks), lp->pagein_calls,
		    lp->pagein_bytes, (unsigned long long)apfs_ticks_ms(lp->pagein_ticks));
	}
}

/*
 * kern.apfs_stats: reading it says where the time went, writing anything zeroes the
 * counters.  The point is to tell apart, on one road through the driver, the time spent
 * waiting for the device, the time spent decompressing, and the rest -- which is the
 * driver's own work and the paging machinery above it.
 */
static int
sysctl_apfs_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct apfs_mount *am = apfs_stats_mount();
	size_t cap = 16384;
	char *text = kalloc_data(cap, Z_WAITOK | Z_ZERO);
	int error;

	if (text == NULL) {
		return ENOMEM;
	}
	if (am == NULL) {
		strlcpy(text, "apfs: nothing mounted\n", cap);
	} else {
		struct apfs_nx *nx = &am->am_nx;

		snprintf(text, cap,
		    "pagein   %llu calls  %llu ms  %llu bytes\n"
		    "read     %llu calls  %llu ms  %llu bytes; from the page cache %llu, read into it %llu (%s)\n"
		    "device   %llu calls  %llu ms  %llu bytes\n"
		    "data     %llu calls  %llu ms  %llu bytes\n"
		    "inflate  %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n"
		    "blocks   %llu read  %llu cache hits\n"
		    "lookups  %llu by the name's hash  %llu by reading the directory\n"
		    "lock     %llu takes  %llu ms waiting  %llu ms held; metadata read without it %llu times\n",
		    (unsigned long long)am->am_pagein_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
		    (unsigned long long)am->am_pagein_bytes,
		    (unsigned long long)am->am_read_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
		    (unsigned long long)am->am_read_bytes,
		    (unsigned long long)am->am_read_cached,
		    (unsigned long long)am->am_read_filled,
		    am->am_ubc_read ? "read(2) through it" : "read(2) past it: apfs_ubc_read=0",
		    (unsigned long long)nx->dev_calls,
		    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
		    (unsigned long long)nx->dev_bytes,
		    (unsigned long long)nx->data_calls,
		    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
		    (unsigned long long)nx->data_bytes,
		    (unsigned long long)nx->inflate_calls,
		    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
		    (unsigned long long)nx->inflate_in,
		    (unsigned long long)nx->inflate_out,
		    (unsigned long long)nx->inflate_fallbacks,
		    (unsigned long long)nx->reads,
		    (unsigned long long)nx->hits,
		    (unsigned long long)nx->lookups_hashed,
		    (unsigned long long)nx->lookups_scanned,
		    (unsigned long long)am->am_lock_waits,
		    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
		    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks),
		    (unsigned long long)nx->unlocked_fills);
		apfs_lock_report(am, text, cap);
	}
	error = SYSCTL_OUT(req, text, strlen(text) + 1);
	kfree_data(text, cap);
	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (am != NULL) {
		apfs_lock(am);
		am->am_pagein_calls = am->am_pagein_ticks = am->am_pagein_bytes = 0;
		am->am_read_calls = am->am_read_ticks = am->am_read_bytes = 0;
		am->am_nx.dev_calls = am->am_nx.dev_ticks = am->am_nx.dev_bytes = 0;
		am->am_nx.data_calls = am->am_nx.data_ticks = am->am_nx.data_bytes = 0;
		am->am_nx.inflate_calls = am->am_nx.inflate_ticks = 0;
		am->am_nx.inflate_in = am->am_nx.inflate_out = am->am_nx.inflate_fallbacks = 0;
		am->am_nx.reads = am->am_nx.hits = 0;
		am->am_lock_waits = am->am_lock_wait_ticks = am->am_lock_held_ticks = 0;
		memset(am->am_lock_wait_hist, 0, sizeof(am->am_lock_wait_hist));
		am->am_lock_wait_max = 0;
		memset(am->am_lock_hold_hist, 0, sizeof(am->am_lock_hold_hist));
		am->am_lock_hold_max = 0;
		memset(am->am_waiters_hist, 0, sizeof(am->am_waiters_hist));
		am->am_lookup_negative = 0;
		am->am_lookup_negative_hits = 0;
		am->am_inactive_free = 0;
		am->am_lookup_positive_hits = am->am_node_found = am->am_node_made = am->am_node_named = 0;
		memset(am->am_prio_wait, 0, sizeof(am->am_prio_wait));
		memset(am->am_prio_long, 0, sizeof(am->am_prio_long));
		memset(am->am_prio_long_wait, 0, sizeof(am->am_prio_long_wait));
		memset(am->am_long_blocked, 0, sizeof(am->am_long_blocked));
		memset(am->am_long_queued, 0, sizeof(am->am_long_queued));
		memset(am->am_long_oncpu, 0, sizeof(am->am_long_oncpu));
		memset(am->am_long_cpu, 0, sizeof(am->am_long_cpu));
		lck_spin_lock(&am->am_procs_spin);
		if (am->am_procs != NULL) {
			memset(am->am_procs, 0, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
		}
		am->am_nprocs = 0;
		lck_spin_unlock(&am->am_procs_spin);
		am->am_read_cached = am->am_read_filled = 0;
		for (uint32_t i = 0; i < APFS_LOCK_SITES; i++) {
			const char *name = am->am_sites[i].name;

			memset(&am->am_sites[i], 0, sizeof(am->am_sites[i]));
			am->am_sites[i].name = name;
		}
		apfs_unlock(am);
	}
	return 0;
}

/*
 * The same breakdown, straight to the log.  Needed where there is no user space to read
 * the sysctl: when the volume is the root, nothing else is running.
 */
void
apfs_stats_print(void)
{
	struct apfs_mount *am = apfs_stats_mount();
	struct apfs_nx *nx;

	if (am == NULL) {
		printf("apfs: nothing mounted\n");
		return;
	}
	nx = &am->am_nx;
	printf("apfs: pagein  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_pagein_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
	    (unsigned long long)am->am_pagein_bytes);
	printf("apfs: read    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_read_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
	    (unsigned long long)am->am_read_bytes);
	printf("apfs: device  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->dev_calls,
	    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
	    (unsigned long long)nx->dev_bytes);
	/* file contents, read without the lock (the line above is metadata, with it) */
	printf("apfs: data    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->data_calls,
	    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
	    (unsigned long long)nx->data_bytes);
	/* "by zlib": streams libdeflate refused (bad, or more out than the piece holds), zlib's then */
	printf("apfs: inflate %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n",
	    (unsigned long long)nx->inflate_calls,
	    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
	    (unsigned long long)nx->inflate_in, (unsigned long long)nx->inflate_out,
	    (unsigned long long)nx->inflate_fallbacks);
	/* requests whose whole pieces went out together (nx->parallel), and how many pieces */
	printf("apfs: batch   %llu requests  %llu pieces  %u threads at most\n",
	    (unsigned long long)nx->batch_calls, (unsigned long long)nx->batch_pieces,
	    nx->parallel != NULL ? nx->parallel_max : 1);
	printf("apfs: ahead   %llu times  %llu pieces  %llu of them used\n",
	    (unsigned long long)nx->ra_batches, (unsigned long long)nx->ra_pieces,
	    (unsigned long long)nx->ra_hits);
	printf("apfs: blocks  %llu read  %llu cache hits  highest block %llu\n",
	    (unsigned long long)nx->reads, (unsigned long long)nx->hits,
	    (unsigned long long)nx->max_addr);
	printf("apfs: lock    %llu takes  %llu ms waiting  %llu ms held\n",
	    (unsigned long long)am->am_lock_waits,
	    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
	    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks));
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_wait_max, &max_ns);
		printf("apfs: lock    waits: %llu under 2 us, %llu to 10 us, %llu to 100 us, %llu to 1 ms, %llu longer; "
		    "longest %llu us; metadata read from the device %llu ms\n",
		    am->am_lock_wait_hist[0], am->am_lock_wait_hist[1], am->am_lock_wait_hist[2], am->am_lock_wait_hist[3],
		    am->am_lock_wait_hist[4], max_ns / 1000, (unsigned long long)apfs_ticks_ms(nx->dev_ticks));
		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		printf("apfs: lock    holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to 10 ms, %llu longer; "
		    "longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1], am->am_lock_hold_hist[2],
		    am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
		printf("apfs: lookups %llu by the name's hash, %llu by reading the directory\n", nx->lookups_hashed,
		    nx->lookups_scanned);
		{
			char *rep = kalloc_data(16384, Z_WAITOK | Z_ZERO);

			if (rep != NULL) {
				apfs_lock_report(am, rep, 16384);
				printf("%s", rep);
				kfree_data(rep, 16384);
			}
		}
		printf("apfs: metadata read without the lock: %llu times, %llu of them already in by another's read; "
		    "retired blocks %llu now, %llu at most; operations going on now %u\n", nx->unlocked_fills, nx->fill_races,
		    nx->retired_now, nx->retired_most, am->am_nops);
	}
}

/* kern.redwood_memory: the kernel's line about memory (osfmk/arm64/loader_return.c), for a job list to keep */
extern void redwood_memory_line(char *buf, size_t len);

static int
sysctl_redwood_memory SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[256];

	redwood_memory_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, redwood_memory, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_redwood_memory, "A", "where the memory is, in MB");

SYSCTL_PROC(_kern, OID_AUTO, apfs_stats, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_apfs_stats, "A", "APFS: where the time goes; write to zero it");

/* ------------------------------------------------------------------ vnodes */

static uint32_t
apfs_hash_index(uint64_t obj_id)
{
	/* the low bits of an identifier are the ones that differ between neighbours */
	return (uint32_t)((obj_id ^ (obj_id >> 17)) % APFS_HASH_BUCKETS);
}

/* The vnode for an object, making one if there is not one already. */
int
apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam vfsp;
	struct apfs_node *an;
	struct apfs_inode ino;
	uint32_t b = apfs_hash_index(obj_id);
	uint64_t size = 0;
	enum vtype vtype;
	int error;

	/* already have one? */
	apfs_lock(am);
	for (an = am->am_hash[b]; an != NULL; an = an->an_next) {
		if (an->an_id == obj_id && an->an_attached) {
			vnode_t vp = an->an_vp;
			uint32_t vid = vnode_vid(vp);

			apfs_unlock(am);
			if (vnode_getwithvid(vp, vid) == 0) {
				os_atomic_inc(&am->am_node_found, relaxed);
				/*
				 * Found by a lookup: the name goes into the name cache, as vnode_create
				 * puts it there for a new vnode -- else every later lookup of this
				 * name comes back here for the lock.  (The volume does not change;
				 * a synthetic name is looked for only where there is no real one, and
				 * adding one purges its directory's entries.)  The name cache keeps
				 * one name per vnode and leaves one that has a name already as it is.
				 */
				if (dvp != NULLVP && cnp != NULL && (cnp->cn_flags & MAKEENTRY)) {
					cache_enter(dvp, vp, cnp);
					os_atomic_inc(&am->am_node_named, relaxed);
				}
				*vpp = vp;
				return 0;
			}
			/* it was going away under us: fall through and make a new one */
			apfs_lock(am);
			break;
		}
	}

	if (obj_id >= APFS_SYNTH_BASE) {
		/* a synthetic directory: an empty one, root's, made up here */
		const struct apfs_synth *sy = NULL;
		uint32_t i;

		for (i = 0; i < am->am_nsynth; i++) {
			if (am->am_synth[i].id == obj_id) {
				sy = &am->am_synth[i];
			}
		}
		bzero(&ino, sizeof(ino));
		if (sy == NULL) {
			error = ENOENT;
		} else {
			ino.obj_id = obj_id;
			ino.parent_id = sy->parent;
			ino.mode = APFS_S_IFDIR | 0755;
			error = 0;
		}
	} else {
		error = apfs_inode_get(&am->am_vol, obj_id, &ino);
		if (error == 0) {
			uint32_t i;

			error = apfs_inode_size(&am->am_vol, &ino, &size);
			if (error) {
				size = ino.size;       /* the stream's size is still better than nothing */
				error = 0;
			}
			/* a directory with another name: ".." is where that name is */
			for (i = 0; i < am->am_nsynth; i++) {
				if (am->am_synth[i].alias && am->am_synth[i].id == obj_id) {
					ino.parent_id = am->am_synth[i].parent;
				}
			}
		}
	}
	apfs_unlock(am);
	if (error) {
		return error;
	}

	switch (ino.mode & APFS_S_IFMT) {
	case APFS_S_IFREG:  vtype = VREG; break;
	case APFS_S_IFDIR:  vtype = VDIR; break;
	case APFS_S_IFLNK:  vtype = VLNK; break;
	case APFS_S_IFCHR:  vtype = VCHR; break;
	case APFS_S_IFBLK:  vtype = VBLK; break;
	case APFS_S_IFIFO:  vtype = VFIFO; break;
	case APFS_S_IFSOCK: vtype = VSOCK; break;
	default:
		return EIO;
	}

	an = kalloc_type(struct apfs_node, Z_WAITOK | Z_ZERO);
	if (an == NULL) {
		return ENOMEM;
	}
	an->an_mount = am;
	an->an_id = obj_id;
	an->an_parent = ino.parent_id;
	an->an_mode = ino.mode;
	an->an_size = size;
	an->an_ino = ino;       /* nothing here can change: the mount is read only */

	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = am->am_mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "apfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = an;
	vfsp.vnfs_vops = apfs_vnodeop_p;
	vfsp.vnfs_markroot = (obj_id == am->am_root_id);
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_rdev = (vtype == VCHR || vtype == VBLK) ? (dev_t)ino.rdev : 0;
	vfsp.vnfs_filesize = size;
	vfsp.vnfs_cnp = cnp;
	/* let VFS cache the name only when it came from a lookup that has one */
	vfsp.vnfs_flags = (dvp != NULL && cnp != NULL) ? 0 : VNFS_NOCACHE;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &an->an_vp);
	if (error) {
		kfree_type(struct apfs_node, an);
		return error;
	}
	vnode_settag(an->an_vp, VT_OTHER);

	apfs_lock(am);
	an->an_next = am->am_hash[b];
	am->am_hash[b] = an;
	an->an_attached = true;
	apfs_unlock(am);
	os_atomic_inc(&am->am_node_made, relaxed);

	*vpp = an->an_vp;
	return 0;
}

/* A synthetic directory by where it is and its name; with am_lock held. */
const struct apfs_synth *
apfs_synth_find(struct apfs_mount *am, uint64_t parent, const char *name, size_t len)
{
	uint32_t i;

	for (i = 0; i < am->am_nsynth; i++) {
		if (am->am_synth[i].parent == parent && strlen(am->am_synth[i].name) == len &&
		    memcmp(am->am_synth[i].name, name, len) == 0) {
			return &am->am_synth[i];
		}
	}
	return NULL;
}

/*
 * sysctl kern.apfs_synthetic=PATH: a synthetic directory at PATH, whose parent must be a
 * directory of an APFS volume (real or synthetic) and which must not exist yet.
 * PATH=TARGET: at PATH, another name for the directory TARGET of the same volume.  Root
 * only; nothing ever takes one away again but unmounting.
 */
static int
apfs_sysctl_synthetic SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char path[MAXPATHLEN], name[APFS_SYNTH_NAMELEN], *target;
	vnode_t dvp = NULLVP, vp = NULLVP, tvp = NULLVP;
	struct apfs_mount *am;
	struct apfs_node *dn;
	uint64_t target_id = 0;
	const char *slash;
	size_t len;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, "", 1);          /* reading it says nothing */
	}
	if ((error = suser(kauth_cred_get(), NULL)) != 0) {
		return error;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(path)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, path, req->newlen);
	if (error) {
		return error;
	}
	path[req->newlen] = '\0';
	target = strchr(path, '=');
	if (target != NULL) {
		*target++ = '\0';
		error = vnode_lookup(target, 0, &tvp, vfs_context_current());
		if (error) {
			return error;
		}
		if (vnode_vtype(tvp) != VDIR || vnode_mount(tvp) == NULL ||
		    strcmp(vfs_statfs(vnode_mount(tvp))->f_fstypename, "apfs") != 0) {
			vnode_put(tvp);
			return ENOTDIR;
		}
		target_id = APFS_VP(tvp)->an_id;
	}
	while ((len = strlen(path)) > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
	}
	slash = strrchr(path, '/');
	if (slash == NULL || slash[1] == '\0') {
		error = EINVAL;
		goto out;
	}
	len = strlen(slash + 1);
	if (len >= sizeof(name)) {
		error = EINVAL;
		goto out;
	}
	strlcpy(name, slash + 1, sizeof(name));        /* before the path is cut to its parent */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		error = EINVAL;
		goto out;
	}
	if (vnode_lookup(path, 0, &vp, vfs_context_current()) == 0) {
		vnode_put(vp);
		error = EEXIST;
		goto out;
	}
	path[slash - path == 0 ? 1 : slash - path] = '\0';     /* the parent ("/" for "/name") */
	error = vnode_lookup(path, 0, &dvp, vfs_context_current());
	if (error) {
		goto out;
	}
	if (vnode_vtype(dvp) != VDIR || vnode_mount(dvp) == NULL ||
	    strcmp(vfs_statfs(vnode_mount(dvp))->f_fstypename, "apfs") != 0 ||
	    (tvp != NULLVP && vnode_mount(tvp) != vnode_mount(dvp))) {
		vnode_put(dvp);
		error = ENOTSUP;
		goto out;
	}
	dn = APFS_VP(dvp);
	am = dn->an_mount;
	apfs_lock(am);
	if (am->am_synth == NULL) {
		am->am_synth = kalloc_data(APFS_SYNTH_MAX * sizeof(*am->am_synth), Z_WAITOK | Z_ZERO);
	}
	if (am->am_synth == NULL) {
		error = ENOMEM;
	} else if (apfs_synth_find(am, dn->an_id, name, len) != NULL) {
		error = EEXIST;
	} else if (am->am_nsynth >= APFS_SYNTH_MAX) {
		error = ENOSPC;
	} else {
		uint32_t n0 = am->am_nsynth;
		uint64_t id = (tvp != NULLVP) ? target_id : APFS_SYNTH_BASE + n0;
		struct apfs_inode ino;

		/* an id the volume does not use itself, or nothing (reading the tree may let go of the lock) */
		if (tvp == NULLVP && apfs_inode_get(&am->am_vol, id, &ino) != ENOENT) {
			error = EEXIST;
		} else if (am->am_nsynth != n0 || apfs_synth_find(am, dn->an_id, name, len) != NULL) {
			error = EBUSY;          /* another was made meanwhile: try again */
		} else {
			struct apfs_synth *sy = &am->am_synth[am->am_nsynth];

			sy->parent = dn->an_id;
			sy->alias = (tvp != NULLVP);
			sy->id = id;
			strlcpy(sy->name, name, sizeof(sy->name));
			am->am_nsynth++;
			error = 0;
		}
	}
	apfs_unlock(am);
	/* the parent's entries changed: what the name cache has of it is stale */
	cache_purge(dvp);
	cache_purge_negatives(dvp);     /* (the new name was known not to be there) */
	vnode_put(dvp);
out:
	if (tvp != NULLVP) {
		vnode_put(tvp);
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, apfs_synthetic, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, apfs_sysctl_synthetic, "A",
    "write PATH: an empty synthetic directory on an APFS volume (a mount point)");

/* Take a node out of the hash; called from vnop_reclaim. */
void
apfs_node_detach(struct apfs_node *an)
{
	struct apfs_mount *am = an->an_mount;
	uint32_t b = apfs_hash_index(an->an_id);
	struct apfs_node **pp;

	apfs_lock(am);
	for (pp = &am->am_hash[b]; *pp != NULL; pp = &(*pp)->an_next) {
		if (*pp == an) {
			*pp = an->an_next;
			break;
		}
	}
	an->an_attached = false;
	apfs_unlock(am);
}

/* apfs_lib lets go of the mount's lock round decompression, through these */
static void
apfs_lib_unlock(void *ctx)
{
	apfs_unlock_inner((struct apfs_mount *)ctx);     /* the operation goes on */
}

static void
apfs_lib_relock(void *ctx)
{
	apfs_lock_inner((struct apfs_mount *)ctx);
}

/* ------------------------------------------------------- threads for one request */

/*
 * apfs_lib's nx->parallel: a few kernel threads that help a request of many compressed
 * pieces along, each reading and decompressing whole pieces into the caller's buffer while
 * the caller does the same.  A request posts a job and wakes as many helpers as it can
 * use; a helper that comes takes a worker number and then items until none are left.  The
 * caller works through items too, then takes its job off the list -- no helper can join
 * after that -- and waits for the helpers still inside it.  The job lives on the caller's
 * stack, which is why nothing may touch it once the caller is back.
 *
 * Boot-arg apfs_par=N: at most N threads on one request, the caller's included (default:
 * four, or the processors if fewer -- see apfs_pool_start(); 1 turns the helpers off).
 */
struct apfs_pjob {
	struct apfs_pjob *pj_next;
	int             (*pj_fn)(void *arg, uint32_t item, uint32_t worker);
	void             *pj_arg;
	uint32_t          pj_items;
	uint32_t          pj_workers;
	volatile SInt32   pj_taken;     /* items handed out so far (may run past pj_items) */
	volatile SInt32   pj_error;
	uint32_t          pj_joined;    /* worker numbers given out; under apfs_pool_lock */
	uint32_t          pj_active;    /* helpers inside the job now; under apfs_pool_lock */
};

static struct apfs_pjob *apfs_pool_jobs;        /* oldest first */
static uint32_t apfs_pool_threads;
static uint32_t apfs_par_max;                   /* threads on one request, the caller's included */

static void
apfs_pjob_work(struct apfs_pjob *pj, uint32_t worker)
{
	for (;;) {
		SInt32 item = OSIncrementAtomic(&pj->pj_taken);
		int error;

		if (item < 0 || (uint32_t)item >= pj->pj_items) {
			break;
		}
		error = pj->pj_fn(pj->pj_arg, (uint32_t)item, worker);
		if (error != 0) {
			OSCompareAndSwap(0, (UInt32)error, (volatile UInt32 *)&pj->pj_error);
		}
	}
}

static void
apfs_pool_thread(__unused void *arg, __unused wait_result_t wr)
{
	lck_mtx_lock(&apfs_pool_lock);
	for (;;) {
		struct apfs_pjob *pj;
		uint32_t worker;

		for (pj = apfs_pool_jobs; pj != NULL; pj = pj->pj_next) {
			if (pj->pj_joined < pj->pj_workers &&
			    (uint32_t)pj->pj_taken < pj->pj_items) {
				break;
			}
		}
		if (pj == NULL) {
			(void)msleep(&apfs_pool_jobs, &apfs_pool_lock, PRIBIO, "apfs_pool", NULL);
			continue;
		}
		worker = pj->pj_joined++;
		pj->pj_active++;
		lck_mtx_unlock(&apfs_pool_lock);
		apfs_pjob_work(pj, worker);
		lck_mtx_lock(&apfs_pool_lock);
		if (--pj->pj_active == 0) {
			wakeup(&pj->pj_active);
		}
	}
}

static int
apfs_parallel(__unused void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg)
{
	struct apfs_pjob pj, **pp;
	uint32_t i;

	bzero(&pj, sizeof(pj));
	pj.pj_fn = fn;
	pj.pj_arg = arg;
	pj.pj_items = items;
	pj.pj_workers = workers;
	pj.pj_joined = 1;               /* the caller is worker 0 */

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != NULL; pp = &(*pp)->pj_next) {
	}
	*pp = &pj;
	for (i = 1; i < workers; i++) {
		wakeup_one((caddr_t)&apfs_pool_jobs);
	}
	lck_mtx_unlock(&apfs_pool_lock);

	apfs_pjob_work(&pj, 0);

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != &pj; pp = &(*pp)->pj_next) {
	}
	*pp = pj.pj_next;
	while (pj.pj_active > 0) {
		(void)msleep(&pj.pj_active, &apfs_pool_lock, PRIBIO, "apfs_pjob", NULL);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return pj.pj_error;
}

/*
 * The helpers, once, at the first mount.  Four threads on one request at most unless
 * apfs_par says otherwise: on this phone one reader with 1 MB reads took 765 ms over the
 * restore ramdisk with two, 612 with four or five, 714 with all eight -- the four small
 * cores take two and a half times as long over a piece, and a request waits for its
 * slowest one.
 */
static void
apfs_pool_start(void)
{
	uint32_t want = ml_get_topology_info()->num_cpus, i;

	if (want > 4) {
		want = 4;
	}
	if (want < 1) {
		want = 1;
	}
	PE_parse_boot_argn("apfs_par", &want, sizeof(want));
	if (want < 1) {
		want = 1;
	}
	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_par_max != 0) {
		lck_mtx_unlock(&apfs_pool_lock);
		return;
	}
	apfs_par_max = want;
	lck_mtx_unlock(&apfs_pool_lock);
	for (i = 0; i + 1 < want; i++) {
		thread_t th;

		if (kernel_thread_start(apfs_pool_thread, NULL, &th) != KERN_SUCCESS) {
			break;
		}
		thread_deallocate(th);
		OSIncrementAtomic((volatile SInt32 *)&apfs_pool_threads);
	}
	printf("apfs: %u threads on a request of many pieces (%u helpers)\n", want,
	    apfs_pool_threads);
}

/* ------------------------------------------------------------------ mount */

static void
apfs_free_mount(struct apfs_mount *am)
{
	if (am->am_synth != NULL) {
		kfree_data(am->am_synth, APFS_SYNTH_MAX * sizeof(*am->am_synth));
	}
	while (am->am_iobuf_kept > 0) {
		kfree_data(am->am_iobuf[--am->am_iobuf_kept], am->am_iobuf_size);
	}
	apfs_nx_close(&am->am_nx);
	if (am->am_procs != NULL) {
		kfree_data(am->am_procs, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
	}
	lck_spin_destroy(&am->am_iobuf_spin, &apfs_lck_grp);
	lck_spin_destroy(&am->am_procs_spin, &apfs_lck_grp);
	lck_mtx_destroy(&am->am_lock, &apfs_lck_grp);
	kfree_type(struct apfs_mount, am);
}

/*
 * Which container of the device and which volume of it: containers lie back to back, each
 * saying in its superblock how big it is (nx_block_count blocks of nx_block_size), so the
 * N-th starts where the one before it ends -- the RAM disks of this port hold several (the
 * boot volume, a cryptex) and so may the phone's darwin partition.  volume < 0: the first
 * that has a root directory.
 */
static int
apfs_mount_common(mount_t mp, vnode_t devvp, vfs_context_t ctx, uint32_t container, int volume)
{
	struct apfs_mount *am;
	struct apfs_devctx *dc;
	struct apfs_dev dev;
	struct vfsstatfs *sp;
	uint32_t devblock = 0;
	uint64_t devblocks = 0, nx_size = 0;
	int error;

	if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&devblock, 0, ctx) != 0 ||
	    devblock == 0 || (APFS_META_CHUNK % devblock) != 0) {
		return EINVAL;
	}
	(void)VNOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT, (caddr_t)&devblocks, 0, ctx);

	am = kalloc_type(struct apfs_mount, Z_WAITOK | Z_ZERO);
	if (am == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&am->am_lock, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_iobuf_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_procs_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	{
		uint32_t ubc = 1;

		(void)PE_parse_boot_argn("apfs_ubc_read", &ubc, sizeof(ubc));
		am->am_ubc_read = (ubc != 0);
	}
	am->am_procs = kalloc_data(APFS_LOCK_PROCS * sizeof(am->am_procs[0]), Z_WAITOK | Z_ZERO);
	am->am_mp = mp;
	am->am_devvp = devvp;
	am->am_devblock = devblock;

	dc = kalloc_type(struct apfs_devctx, Z_WAITOK | Z_ZERO);
	if (dc == NULL) {
		apfs_free_mount(am);
		return ENOMEM;
	}
	dc->dc_devvp = devvp;
	dc->dc_devblock = devblock;
	{
		uint8_t *sb = kalloc_data(APFS_META_CHUNK, Z_WAITOK);
		uint64_t devsize = devblocks * (uint64_t)devblock;
		uint32_t i;

		error = (sb == NULL) ? ENOMEM : 0;
		for (i = 0; error == 0; i++) {
			uint32_t bs;
			uint64_t bc;

			error = apfs_dev_read(dc, 0, sb, APFS_META_CHUNK);
			if (error == 0 && memcmp(sb + 32, "NXSB", 4) != 0) {
				error = EINVAL;         /* (no container here: as the root, the next filesystem's turn) */
			}
			if (error) {
				break;
			}
			bs = (uint32_t)sb[36] | (uint32_t)sb[37] << 8 | (uint32_t)sb[38] << 16 | (uint32_t)sb[39] << 24;
			bc = 0;
			for (int b = 7; b >= 0; b--) {
				bc = (bc << 8) | sb[40 + b];
			}
			nx_size = bc * bs;
			if (bs < 4096 || bc == 0 || (nx_size % devblock) != 0 ||
			    (devsize != 0 && dc->dc_base + nx_size > devsize)) {
				printf("apfs: container %u: %llu blocks of %u do not fit the device\n", i,
				    (unsigned long long)bc, bs);
				error = EINVAL;
				break;
			}
			if (i == container) {
				break;
			}
			dc->dc_base += nx_size;
		}
		if (sb != NULL) {
			kfree_data(sb, APFS_META_CHUNK);
		}
		if (error) {
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return error;
		}
	}

	bzero(&dev, sizeof(dev));
	dev.ctx = dc;
	dev.size = nx_size;
	dev.read = apfs_dev_read;
	dev.read_data = apfs_dev_read_data;

	error = apfs_nx_open(&am->am_nx, &dev);
	if (error) {
		/*
		 * EINVAL means the magic was not there, so this is not an APFS container at
		 * all: as the root filesystem that lets vfs_mountroot() try the next one
		 * without a word.
		 */
		if (error != EINVAL) {
			printf("apfs: cannot read the container: %d\n", error);
		}
		kfree_type(struct apfs_devctx, dc);
		apfs_free_mount(am);
		return error;
	}

	/* apfs_nx_open() starts from a clean slate, so the lock is handed over only now */
	am->am_nx.unlock = apfs_lib_unlock;
	am->am_nx.relock = apfs_lib_relock;
	am->am_nx.lock_ctx = am;
	am->am_iobuf_size = APFS_IOBUF_DEFAULT;
	{
		uint32_t round = 0;

		if (PE_parse_boot_argn("apfs_round", &round, sizeof(round)) &&
		    round >= 64 * 1024 && round <= APFS_IOBUF_MAX && (round % (64 * 1024)) == 0) {
			am->am_iobuf_size = round;
		}
	}
	(void)PE_parse_boot_argn("apfs_meta_cluster", &am->am_nx.meta_cluster,
	    sizeof(am->am_nx.meta_cluster));
	/*
	 * The phone's flash and QEMU's virtio disk cost tens of microseconds a request; this
	 * port's other disks (md, the resident image; rsp, part of an image) answer with a
	 * memory copy.  Boot-arg apfs_cheap=0/1 says otherwise.
	 */
	{
		dev_t rdev = vnode_specrdev(devvp);
		uint32_t cheap;

		am->am_nx.cheap_requests = !redwood_ufs_dev(rdev) && !redwood_virtio_blk_dev(rdev);
		if (PE_parse_boot_argn("apfs_cheap", &cheap, sizeof(cheap))) {
			am->am_nx.cheap_requests = (cheap != 0);
		}
		printf("apfs: device %d,%d: requests to it %s\n", major(rdev), minor(rdev),
		    am->am_nx.cheap_requests ? "cost next to nothing (a disk in memory)" :
		    "are worth putting together");
	}
	apfs_pool_start();
	if (apfs_pool_threads > 0) {
		am->am_nx.parallel = apfs_parallel;
		am->am_nx.parallel_max = apfs_pool_threads + 1;
	}

	/* the first volume that has a root directory */
	{
		uint32_t i;
		bool got = false;

		/* with the lock, as every call into apfs_lib from now on (it may let go of it) */
		apfs_lock(am);
		for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS && !got; i++) {
			struct apfs_inode ino;

			if (am->am_nx.fs_oid[i] == 0 || (volume >= 0 && i != (uint32_t)volume)) {
				continue;
			}
			if (apfs_vol_open(&am->am_nx, i, &am->am_vol) != 0) {
				continue;
			}
			if (apfs_inode_get(&am->am_vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
				got = true;
			}
		}
		apfs_unlock(am);
		if (!got) {
			printf("apfs: no readable volume with a root directory\n");
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return EINVAL;
		}
	}
	am->am_root_id = APFS_ROOT_DIR_INO_NUM;

	vfs_setfsprivate(mp, am);
	vfs_setflags(mp, MNT_RDONLY | MNT_LOCAL);
	vfs_setlocklocal(mp);

	sp = vfs_statfs(mp);
	sp->f_bsize = am->am_nx.block_size;
	sp->f_iosize = am->am_nx.block_size;
	sp->f_blocks = am->am_nx.block_count;
	sp->f_bfree = 0;
	sp->f_bavail = 0;
	sp->f_bused = am->am_vol.alloc_count;
	sp->f_files = am->am_vol.num_files + am->am_vol.num_dirs;
	sp->f_ffree = 0;
	/* (several volumes of one device are told apart by which container and volume) */
	sp->f_fsid.val[0] = (int32_t)vnode_specrdev(devvp);
	sp->f_fsid.val[1] = (int32_t)((vfs_typenum(mp) & 0xff) | (container << 8) | (am->am_vol.index << 20));
	strlcpy(sp->f_fstypename, "apfs", sizeof(sp->f_fstypename));

	apfs_the_mount = am;
	am->am_rdev = vnode_specrdev(devvp);
	lck_mtx_lock(&apfs_pool_lock);
	am->am_next = apfs_mounts;
	apfs_mounts = am;
	lck_mtx_unlock(&apfs_pool_lock);
	printf("apfs: volume %u \"%s\" of container %u (at %llu MB): %llu files, %llu directories, %llu blocks of %u used\n",
	    am->am_vol.index, am->am_vol.name, container, (unsigned long long)(dc->dc_base >> 20), (unsigned long long)am->am_vol.num_files,
	    (unsigned long long)am->am_vol.num_dirs,
	    (unsigned long long)am->am_vol.alloc_count, am->am_nx.block_size);
	return 0;
}

/*
 * As the root filesystem: EINVAL lets vfs_mountroot() try the next quietly.  Boot-args
 * apfs_root_c=N and apfs_root_v=M say which container of the device and which volume of it
 * (default: the first container, its first volume with a root directory).
 */
int
apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
	uint32_t c = 0;
	int v = -1;

	(void)PE_parse_boot_argn("apfs_root_c", &c, sizeof(c));
	(void)PE_parse_boot_argn("apfs_root_v", &v, sizeof(v));
	return apfs_mount_common(mp, devvp, ctx, c, v);
}

/*
 * mount(2), and kernel_mount().  The mount data is the path of the block device holding
 * the container, as a string.  VFS does not resolve it for us the way it does for a
 * filesystem marked VFC_VFSLOCALARGS -- see the note in vfs_conf.c for why this one is
 * not -- so it is looked up and opened here, and closed again in apfs_unmount().
 */
static int
apfs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	char *path;
	vnode_t vp = NULLVP;
	size_t len = 0;
	uint32_t container = 0;
	int volume = -1;
	int error;

	if (vfs_isupdate(mp)) {
		/* nothing about a mounted volume can change, least of all to read-write */
		return vfs_iswriteupgrade(mp) ? EROFS : 0;
	}
	if (devvp != NULLVP) {
		/* mounted as the root: VFS found the device and owns the vnode */
		return apfs_mountroot(mp, devvp, ctx);
	}
	if (data == USER_ADDR_NULL) {
		return EINVAL;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (path == NULL) {
		return ENOMEM;
	}
	if (vfs_iskernelmount(mp)) {
		/* kernel_mount() hands the pointer through untouched */
		error = (strlcpy(path, CAST_DOWN(char *, data), MAXPATHLEN) < MAXPATHLEN)
		    ? 0 : ENAMETOOLONG;
	} else {
		error = copyinstr(data, path, MAXPATHLEN, &len);
	}
	/* "DEVICE[,c=N][,v=M]": the container (from 0) and the volume (by index) */
	if (error == 0) {
		char *opt = strchr(path, ',');

		while (opt != NULL) {
			char *next = strchr(opt + 1, ',');

			*opt = '\0';
			if (strncmp(opt + 1, "c=", 2) == 0) {
				container = (uint32_t)strtoul(opt + 3, NULL, 10);
			} else if (strncmp(opt + 1, "v=", 2) == 0) {
				volume = (int)strtoul(opt + 3, NULL, 10);
			} else {
				error = EINVAL;
			}
			opt = next;
		}
	}
	if (error == 0) {
		error = vnode_lookup(path, 0, &vp, ctx);
	}
	if (error == 0 && !vnode_isblk(vp)) {
		error = ENOTBLK;
	}
	if (error == 0) {
		bool shared = false;

		error = VNOP_OPEN(vp, FREAD, ctx);
		if (error == EBUSY && apfs_device_held(vnode_specrdev(vp))) {
			/*
			 * Mounted already -- by us, another container of it (the root's RAM disk and the
			 * cryptex after the boot volume): read through it as it is open, not opened
			 * again.  Anything else mounted on a device stays EBUSY.
			 */
			shared = true;
			error = 0;
		}
		if (error == 0) {
			/*
			 * The iocount from the lookup is kept for as long as the volume is
			 * mounted; apfs_unmount() closes and drops it.
			 */
			error = apfs_mount_common(mp, vp, ctx, container, volume);
			if (error == 0) {
				APFS_MP(mp)->am_owns_devvp = true;
				APFS_MP(mp)->am_shares_devvp = shared;
				/* what mount(8) and df show as where it came from */
				if (vfs_statfs(mp)->f_mntfromname[0] == '\0' || container != 0 || volume >= 0) {
					char opts[40] = "";

					if (container != 0) {
						snprintf(opts, sizeof(opts), ",c=%u", container);
					}
					if (volume >= 0) {
						snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), ",v=%d", volume);
					}
					snprintf(vfs_statfs(mp)->f_mntfromname, MAXPATHLEN, "%s%s", path, opts);
				}
			} else if (!shared) {
				(void)VNOP_CLOSE(vp, FREAD, ctx);
			}
		}
		if (error != 0) {
			vnode_put(vp);
		}
	} else if (vp != NULLVP) {
		vnode_put(vp);
	}
	kfree_data(path, MAXPATHLEN);
	return error;
}

static int
apfs_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	if (apfs_the_mount == am) {
		apfs_the_mount = NULL;
	}
	lck_mtx_lock(&apfs_pool_lock);
	for (struct apfs_mount **pp = &apfs_mounts; *pp != NULL; pp = &(*pp)->am_next) {
		if (*pp == am) {
			*pp = am->am_next;
			break;
		}
	}
	lck_mtx_unlock(&apfs_pool_lock);
	vfs_setfsprivate(mp, NULL);
	if (am->am_owns_devvp && am->am_devvp != NULLVP) {
		if (!am->am_shares_devvp) {
			(void)VNOP_CLOSE(am->am_devvp, FREAD, ctx);
		}
		vnode_put(am->am_devvp);
	}
	apfs_free_mount(am);
	return 0;
}

static int
apfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);

	return apfs_node_vnode(am, am->am_root_id, NULLVP, NULL, vpp);
}

static int
apfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	return apfs_node_vnode(APFS_MP(mp), (uint64_t)ino, NULLVP, NULL, vpp);
}

static int
apfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	uint64_t files = am->am_vol.num_files + am->am_vol.num_dirs;

	VFSATTR_RETURN(fsap, f_objcount, files);
	VFSATTR_RETURN(fsap, f_bsize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_iosize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_blocks, am->am_nx.block_count);
	VFSATTR_RETURN(fsap, f_bfree, 0);
	VFSATTR_RETURN(fsap, f_bavail, 0);
	VFSATTR_RETURN(fsap, f_bused, am->am_vol.alloc_count);
	VFSATTR_RETURN(fsap, f_files, files);
	VFSATTR_RETURN(fsap, f_ffree, 0);

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;
		bool nocase = (am->am_vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

		bzero(cap, sizeof(*cap));
		/*
		 * An object's identifier never changes and apfs_vget() finds an object by it,
		 * which is what lets fsgetpath(2) and /.vol work for this volume: without it,
		 * what asks for a file's path by its identifier (LaunchServices among them) gets
		 * nothing.
		 */
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    (nocase ? 0 : VOL_CAP_FMT_CASE_SENSITIVE);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, am->am_vol.name, MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, am->am_nx.uuid, sizeof(fsap->f_uuid));
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	return 0;
}

/* Nothing is ever dirty. */
static int
apfs_sync(__unused mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	return 0;
}

static int
apfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops apfs_vfsops = {
	.vfs_mount = apfs_mount,
	.vfs_unmount = apfs_unmount,
	.vfs_root = apfs_root,
	.vfs_getattr = apfs_vfs_getattr,
	.vfs_sync = apfs_sync,
	.vfs_vget = apfs_vget,
	.vfs_init = apfs_init,
};
