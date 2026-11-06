/*
 * Mount the APFS volume on /dev/rvblk0 and walk it through VFS, so that what the driver
 * does can be compared with what tools/apfs/apfs.py reads from the same image on the
 * build machine.  Only with boot-arg apfs_check.
 *
 *   apfs_check=1              walk the whole volume and print only the totals
 *   apfs_check=2              also print a line per entry
 *   apfs_check=3              mount it and stop, leaving it for user space to read
 *                             (userland/src/apfs_mmap_test.c, which is the only way the
 *                             driver's vnop_pagein ever runs)
 *   apfs_check=4              mount nothing: walk the root filesystem, which is what the
 *                             root already is when the volume was handed to the kernel as
 *                             its root device.  This is how the phone is reached: it has
 *                             no driver for its own storage and no virtio, so the only
 *                             block device there is a RAM disk, and a RAM disk given to
 *                             the kernel as the root is mounted by vfs_mountroot.
 *   apfs_check=5              several threads walk the root filesystem at once, each over
 *                             the whole of it, to see what the one lock round apfs_lib
 *                             costs when there is more than one reader
 *   apfs_threads=N            how many, for mode 5 (default 4)
 *   apfs_check=6              mount apfs_dev on apfs_mnt, walk it on this thread and go
 *                             back to the loader.  For a volume that is not the root and
 *                             where there is no user space to make a mount point: the
 *                             point is a directory of the root image itself.
 *   apfs_dev=md1              the device to mount for modes 1 to 3 and 6 (default rvblk0)
 *   apfs_mnt=/lib             where to mount it for mode 6 (default /private/var/apfs-check)
 *   apfs_depth=N              go no deeper than N levels below where the walk starts
 *                             (1 = only the entries of that directory; 0 = no limit).
 *                             For splitting a volume too big to hold into parts: one part
 *                             takes the root's own entries, the others take a subtree
 *                             each, and the per-line hashes add up to the whole.
 *   apfs_max=N                stop after N entries (and say how memory stood on the way):
 *                             for finding out what a walk that never comes back runs into
 *   apfs_skip=/a,/b           directories not to go into, as paths inside the volume,
 *                             comma separated.  Their own line is still counted, only
 *                             their contents are left out -- which is how a volume too
 *                             big to hold is cut into parts that between them cover it
 *                             exactly once.
 *   apfs_subtree=/usr         walk only that part of the volume
 *   apfs_check=7              the one mode that writes: replace bytes inside a file of the
 *                             container on apfs_dev where they lie, with the kernel's own
 *                             bsd/miscfs/apfs/apfs_write.c, and read them back out of a
 *                             container opened again from nothing (apfs_write_check below)
 *   apfs_check=8              the store of fixed-length slot files on apfs_dev: save a payload
 *                             into it and read it back out of a container opened again from
 *                             nothing (apfs_store_check below)
 *
 * A line is the same one tools/apfs/apfs-tool's census and `verify.py clines` print:
 *
 *   path <TAB> d_type <TAB> mode <TAB> uid <TAB> gid <TAB> size <TAB> hash-of-contents
 *
 * The whole volume is 318098 entries, and printing them all through the kernel log would
 * be 47 MB down a serial line, so by default each line is folded into a sum instead: the
 * lines are hashed one at a time and the hashes added together.  Addition does not care
 * what order the entries came in, which matters because this walk and the one on the host
 * need not visit a directory's entries in the same order.  The host adds up the same
 * hashes over its own lines; if the two sums and counts agree, every path, kind, mode,
 * owner, group, size and byte of content agreed.  When they do not, apfs_check=2 over a
 * subtree gives lines to diff.
 *
 * Everything here goes through VFS -- vnode_lookup, vnode_getattr, VNOP_READDIR,
 * VNOP_READLINK, vn_rdwr -- so it exercises the vnode operations and not just the format
 * reading underneath them.
 */

#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/vnode_internal.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>
#include <sys/redwood_apfs_check.h>
#include <miscfs/apfs/apfs_lib.h>
#include <miscfs/apfs/apfs_write.h>
#include <miscfs/apfs/apfs_store.h>
#include <sys/disk.h>
#include <sys/buf.h>
#include <sys/fcntl.h>

extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void apfs_stats_print(void);     /* bsd/miscfs/apfs/apfs_vfsops.c */
extern void redwood_sparse_report(void);

/* how memory stands, for the progress line (osfmk/vm, bsd/vfs) */
extern unsigned int vm_page_free_count, vm_page_wire_count, memorystatus_available_pages;
extern long numvnodes;
extern int desiredvnodes;
#define CHECK_MB(pages) ((unsigned int)(((uint64_t)(pages) * PAGE_SIZE) >> 20))

/* which operation the walk is in, for the stuck detector further down */
static const char *volatile watch_phase = "";
#define PHASE(name) (watch_phase = (name))

#define CHECK_MNT    "/private/var/apfs-check"
#define CHECK_PARENT "/private/var"
#define CHECK_LEAF   "apfs-check"
#define CHECK_DEV    "/dev/rvblk0"

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

/*
 * How much each read asks for: 64 KB, one compressed piece, unless boot-arg apfs_chunk says
 * otherwise (up to 4 MB) -- a bigger read is one request whose pieces the driver can
 * decompress on several processors at once.
 */
static uint32_t
read_chunk(void)
{
	static uint32_t chunk;

	if (chunk == 0) {
		uint32_t want = 64 * 1024;

		PE_parse_boot_argn("apfs_chunk", &want, sizeof(want));
		if (want < 4096 || want > 4 * 1024 * 1024) {
			want = 64 * 1024;
		}
		chunk = want;
	}
	return chunk;
}
#define READ_CHUNK read_chunk()
#define DIR_CHUNK  4096
#define MAX_DEPTH  48
#define PATH_CAP   2048

static uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

struct census {
	vfs_context_t ctx;
	char     *path;                 /* the whole path, starting with CHECK_MNT */
	size_t    rel;                  /* where the part inside the volume starts */
	size_t    len;
	uint8_t  *buf;                  /* for reading file contents */
	uint64_t  sum;                  /* the per-line hashes, added up */
	uint64_t  entries, dirs, files, links, others, bytes;
	uint32_t  errors;
	bool      print_lines;
	int       depth;
	/*
	 * The volume being walked.  Walking the root means walking over the mount points of
	 * other filesystems -- devfs on /dev above all -- and their contents are not this
	 * volume's, so anything that turns out to live on another mount is passed over.
	 */
	mount_t   mp;
	/*
	 * NOCROSSMOUNT when the volume being walked is the root: a lookup then gives the
	 * directory this volume has under a mount point rather than the root of whatever is
	 * mounted on it, so /dev is counted as the (empty) directory it is here instead of
	 * being either skipped or walked as devfs.  Not set when the volume is reached
	 * through a path that itself crosses into it.
	 */
	int       lookup_flags;
	uint32_t  max_depth;            /* 0: no limit */
	char     *skip;                 /* comma separated paths not to descend into */
	uint64_t  max_entries;          /* 0: no limit */
	bool      stop;
	uint64_t  t0;                   /* when the walk started, for the progress line */
	/*
	 * FNV-1a goes a byte at a time, each step waiting on the one before: about three
	 * cycles a byte, more than a second over the restore ramdisk -- the checker's own
	 * time, not the filesystem's.  Boot-arg apfs_hash=0 reads every byte all the same and
	 * does not hash it (the sums are then not the ones to compare); hash_ticks is how
	 * long the hashing took otherwise.
	 */
	bool      no_hash;
	uint64_t  hash_ticks;
	/*
	 * Boot-arg apfs_contents=0: no file is read at all, only looked up and stat'ed --
	 * how long the walk itself takes, the directories and the inodes.
	 */
	bool      no_contents;
	/* apfs_walkers=N: one of N walkers; a subdirectory is left on the shared stack (par_push) */
	bool      par;
	/* where a walker's time goes (mach ticks): lookups, getattr, reading files and links
	 * (hashing included), readdir, waiting for a directory to walk (par); on a processor */
	uint64_t  t_lookup, t_attr, t_read, t_readdir, t_idle, t_cpu, t_wall;
};

/* The boot-args both kinds of walk take the same way. */
static void
census_options(struct census *cs)
{
	uint32_t hash = 1, contents = 1;

	PE_parse_boot_argn("apfs_hash", &hash, sizeof(hash));
	PE_parse_boot_argn("apfs_contents", &contents, sizeof(contents));
	cs->no_hash = (hash == 0);
	cs->no_contents = (contents == 0);
}

/* The format bits of a mode, which vnode_getattr does not report in va_mode. */
static uint16_t
ifmt_of(enum vtype t)
{
	switch (t) {
	case VREG:  return S_IFREG;
	case VDIR:  return S_IFDIR;
	case VLNK:  return S_IFLNK;
	case VCHR:  return S_IFCHR;
	case VBLK:  return S_IFBLK;
	case VFIFO: return S_IFIFO;
	case VSOCK: return S_IFSOCK;
	default:    return 0;
	}
}

/* FNV-1a over a regular file's contents, read through VNOP_READ. */
static int
hash_file(struct census *cs, vnode_t vp, uint64_t size, uint64_t *out)
{
	uint64_t h = FNV_INIT, off = 0;

	while (off < size) {
		int want = (int)((size - off > READ_CHUNK) ? READ_CHUNK : (size - off));
		int resid = 0;
		int error = vn_rdwr(UIO_READ, vp, (caddr_t)cs->buf, want, (off_t)off,
		    UIO_SYSSPACE, IO_NOCACHE, vfs_context_ucred(cs->ctx), &resid,
		    vfs_context_proc(cs->ctx));

		if (error) {
			return error;
		}
		if (resid == want) {
			break;                  /* nothing more to be had */
		}
		if (!cs->no_hash) {
			uint64_t t0 = mach_absolute_time();

			h = fnv1a(cs->buf, (size_t)(want - resid), h);
			cs->hash_ticks += mach_absolute_time() - t0;
		}
		cs->bytes += (uint64_t)(want - resid);
		off += (uint64_t)(want - resid);
	}
	*out = h;
	return 0;
}

/* FNV-1a over a symbolic link's target, read through VNOP_READLINK. */
static int
hash_link(struct census *cs, vnode_t vp, uint64_t *out)
{
	uio_t uio;
	char uio_buf[UIO_SIZEOF(1)];
	int error;
	size_t n;

	uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
	if (uio == NULL) {
		return ENOMEM;
	}
	uio_addiov(uio, CAST_USER_ADDR_T(cs->buf), MAXPATHLEN);
	error = VNOP_READLINK(vp, uio, cs->ctx);
	if (error) {
		return error;
	}
	n = MAXPATHLEN - (size_t)uio_resid(uio);
	cs->bytes += n;
	*out = fnv1a(cs->buf, n, FNV_INIT);
	return 0;
}

static bool
census_entry(struct census *cs, uint8_t d_type)
{
	struct vnode_attr va;
	vnode_t vp = NULLVP;
	uint64_t h = 0, size = 0;
	uint16_t mode = 0;
	char line[PATH_CAP + 128];
	int error;
	int n;

	/*
	 * Do not follow a symbolic link: the point is to look at the link itself, as the host
	 * reader does.  Following them would report the target's kind, size and contents
	 * instead, and a link whose target is missing -- of which this volume has several --
	 * would look like a lookup failure.  Crossing mount points has to stay allowed: the
	 * path leads from the ramfs that holds the mount point into the volume itself.
	 */
	PHASE("lookup");
	{
		uint64_t t0 = mach_absolute_time();

		error = vnode_lookup(cs->path, cs->lookup_flags, &vp, cs->ctx);
		cs->t_lookup += mach_absolute_time() - t0;
	}
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		return false;
	}
	if (cs->mp != NULL && vnode_mount(vp) != cs->mp) {
		vnode_put(vp);          /* another filesystem is mounted here */
		return false;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_uid);
	VATTR_WANTED(&va, va_gid);
	VATTR_WANTED(&va, va_data_size);
	PHASE("getattr");
	{
		uint64_t t0 = mach_absolute_time();

		error = vnode_getattr(vp, &va, cs->ctx);
		cs->t_attr += mach_absolute_time() - t0;
	}
	if (error) {
		printf("apfs-check: %s: getattr failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		goto out;
	}
	mode = (uint16_t)(ifmt_of(vnode_vtype(vp)) | (VATTR_IS_SUPPORTED(&va, va_mode)
	    ? (va.va_mode & 07777) : 0));
	size = VATTR_IS_SUPPORTED(&va, va_data_size) ? va.va_data_size : 0;

	if (vnode_vtype(vp) == VREG) {
		cs->files++;
		PHASE("read");
		if (!cs->no_contents) {
			uint64_t t0 = mach_absolute_time();

			error = hash_file(cs, vp, size, &h);
			cs->t_read += mach_absolute_time() - t0;
		}
	} else if (vnode_vtype(vp) == VLNK) {
		cs->links++;
		PHASE("readlink");
		if (!cs->no_contents) {
			uint64_t t0 = mach_absolute_time();

			error = hash_link(cs, vp, &h);
			cs->t_read += mach_absolute_time() - t0;
		}
	} else if (vnode_vtype(vp) == VDIR) {
		cs->dirs++;
	} else {
		cs->others++;
	}
	if (error) {
		printf("apfs-check: %s: reading it failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		goto out;
	}

	n = scnprintf(line, sizeof(line), "%s\t%u\t%06o\t%u\t%u\t%llu\t%016llx\n",
	    cs->path + cs->rel, d_type, mode,
	    VATTR_IS_SUPPORTED(&va, va_uid) ? va.va_uid : 0,
	    VATTR_IS_SUPPORTED(&va, va_gid) ? va.va_gid : 0,
	    (unsigned long long)size, (unsigned long long)h);
	cs->sum += fnv1a((const uint8_t *)line, (size_t)n, FNV_INIT);
	cs->entries++;
	if (cs->print_lines) {
		printf("apfs-line: %s", line);
	}
	if ((cs->entries % 10000) == 0) {
		uint64_t ns = 0;

		absolutetime_to_nanoseconds(mach_absolute_time() - cs->t0, &ns);
		printf("apfs-check: %llu entries so far at %llu ms  free %u MB  wired %u MB  "
		    "available %u MB  vnodes %ld of %d\n", cs->entries, ns / 1000000,
		    CHECK_MB(vm_page_free_count),
		    CHECK_MB(vm_page_wire_count), CHECK_MB(memorystatus_available_pages),
		    numvnodes, desiredvnodes);
	}
	if (cs->max_entries != 0 && cs->entries >= cs->max_entries) {
		cs->stop = true;
	}
out:
	PHASE("vnode_put");
	vnode_put(vp);
	PHASE("");
	return error == 0;
}

static void census_dir(struct census *cs);
static void par_push(const char *path, size_t len, int depth);

/* the walkers of a parallel walk, for the watch's count of progress */
#define PAR_MAX 16
static struct census *volatile par_cs[PAR_MAX];
static volatile uint32_t par_n;

/*
 * A walk that stops making progress, rather than one that fails.  On the phone a walk of
 * the real volume can stop dead past some tens of thousands of entries, and then nothing
 * is left to say why: the watchdog resets the phone and the log goes with it.  So a timer
 * looks every few seconds; if the count of entries has not moved, it says which entry and
 * which operation the walk is in, and goes back to the loader -- with the log -- before
 * the watchdog does.
 */
#define WATCH_SECONDS 6

static struct census *volatile watch_cs;
static uint64_t watch_last = ~0ULL;
static thread_call_t watch_call;

static void
watch_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	struct census *cs = watch_cs;
	uint64_t deadline, progress;

	if (cs == NULL) {
		return;                 /* the walk is over */
	}
	/* entries and bytes both count as progress: one big file can take a while */
	progress = cs->entries + cs->bytes;
	for (uint32_t i = 0; i < par_n; i++) {
		if (par_cs[i] != NULL) {
			progress += par_cs[i]->entries + par_cs[i]->bytes;
		}
	}
	if (progress == watch_last) {
		printf("apfs-check: STUCK: no progress for %d s after entry %llu, at %s, in %s\n",
		    WATCH_SECONDS, cs->entries, cs->path + cs->rel, watch_phase);
		apfs_stats_print();
		redwood_sparse_report();
		printf("apfs-check: going back to the loader with that\n");
		loader_return();
		return;
	}
	/* a sign the timer got to run at all: a walk can starve every thread call */
	printf("apfs-check: tick, %llu entries\n", cs->entries);
	watch_last = progress;
	clock_interval_to_deadline(WATCH_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(watch_call, deadline);
}

static void
watch_start(struct census *cs)
{
	uint64_t deadline;

	if (watch_call == NULL) {
		watch_call = thread_call_allocate(watch_tick, NULL);
	}
	watch_last = ~0ULL;
	watch_cs = cs;
	clock_interval_to_deadline(WATCH_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(watch_call, deadline);
}

static void
watch_stop(void)
{
	watch_cs = NULL;
	if (watch_call != NULL) {
		thread_call_cancel(watch_call);
	}
}

/* Is this directory one of the ones to stay out of?  Compares the path inside the volume. */
static bool
census_skipped(struct census *cs)
{
	const char *want = cs->path + cs->rel;
	size_t want_len = strlen(want);
	const char *at = cs->skip;

	if (at == NULL) {
		return false;
	}
	while (*at != '\0') {
		const char *end = strchr(at, ',');
		size_t n = (end != NULL) ? (size_t)(end - at) : strlen(at);

		if (n == want_len && memcmp(at, want, n) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		at = end + 1;
	}
	return false;
}

/* Add "/name" to the path, do the entry, and put the path back as it was. */
static void
census_child(struct census *cs, const char *name, size_t name_len, uint8_t d_type)
{
	size_t saved = cs->len;

	if (cs->stop) {
		return;
	}
	if (cs->len + name_len + 2 > PATH_CAP) {
		printf("apfs-check: path below %s is too long\n", cs->path + cs->rel);
		cs->errors++;
		return;
	}
	cs->path[cs->len++] = '/';
	memcpy(cs->path + cs->len, name, name_len);
	cs->len += name_len;
	cs->path[cs->len] = '\0';

	if (census_entry(cs, d_type) && d_type == DT_DIR && cs->depth < MAX_DEPTH &&
	    (cs->max_depth == 0 || (uint32_t)cs->depth + 1 < cs->max_depth) &&
	    !census_skipped(cs)) {
		if (cs->par) {
			par_push(cs->path, cs->len, cs->depth + 1);
		} else {
			cs->depth++;
			census_dir(cs);
			cs->depth--;
		}
	}
	cs->len = saved;
	cs->path[cs->len] = '\0';
}

/* Every entry of the directory the path currently names. */
static void
census_dir(struct census *cs)
{
	vnode_t dvp = NULLVP;
	char *dbuf;
	off_t offset = 0;
	int error;

	{
		uint64_t t0 = mach_absolute_time();

		error = vnode_lookup(cs->len != 0 ? cs->path : "/", cs->lookup_flags, &dvp,
		    cs->ctx);
		cs->t_lookup += mach_absolute_time() - t0;
	}
	if (error == 0 && cs->mp == NULL) {
		cs->mp = vnode_mount(dvp);
	}
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n",
		    cs->len != 0 ? cs->path + cs->rel : "/", error);
		cs->errors++;
		return;
	}
	dbuf = kalloc_data(DIR_CHUNK, Z_WAITOK);
	if (dbuf == NULL) {
		vnode_put(dvp);
		cs->errors++;
		return;
	}
	for (;;) {
		uio_t uio;
		char uio_buf[UIO_SIZEOF(1)];
		int eof = 0, items = 0;
		size_t got;
		char *p;

		uio = uio_createwithbuffer(1, offset, UIO_SYSSPACE, UIO_READ, uio_buf,
		    sizeof(uio_buf));
		if (uio == NULL) {
			cs->errors++;
			break;
		}
		uio_addiov(uio, CAST_USER_ADDR_T(dbuf), DIR_CHUNK);
		PHASE("readdir");
		{
			uint64_t t0 = mach_absolute_time();

			error = VNOP_READDIR(dvp, uio, 0, &eof, &items, cs->ctx);
			cs->t_readdir += mach_absolute_time() - t0;
		}
		if (error) {
			printf("apfs-check: %s: readdir failed: %d\n", cs->path + cs->rel, error);
			cs->errors++;
			break;
		}
		got = DIR_CHUNK - (size_t)uio_resid(uio);
		if (got == 0) {
			break;
		}
		offset = uio_offset(uio);

		for (p = dbuf; p < dbuf + got;) {
			struct dirent *de = (struct dirent *)p;

			if (de->d_reclen == 0 || p + de->d_reclen > dbuf + got) {
				printf("apfs-check: %s: a directory entry of %u bytes\n",
				    cs->path + cs->rel, de->d_reclen);
				cs->errors++;
				break;
			}
			p += de->d_reclen;
			if (de->d_namlen == 0 ||
			    (de->d_namlen == 1 && de->d_name[0] == '.') ||
			    (de->d_namlen == 2 && de->d_name[0] == '.' && de->d_name[1] == '.')) {
				continue;
			}
			census_child(cs, de->d_name, de->d_namlen, de->d_type);
			if (cs->stop) {
				break;
			}
		}
		if (eof || cs->stop) {
			break;
		}
	}
	kfree_data(dbuf, DIR_CHUNK);
	vnode_put(dvp);
}

/*
 * apfs_walkers=N (mode 6): N threads walk the volume at once. They take directories off one
 * shared stack -- a directory's own entries are done by the walker that took it, its
 * subdirectories pushed for any of them -- so that as many files are read at once as there are
 * walkers, and the storage has that many requests to work on. The per-line hashes add up the
 * same whatever the order: the sum and the counts are those of the walk on one thread.
 */
struct par_dir {
	char  *path;
	size_t len;
	int    depth;
};

static LCK_GRP_DECLARE(par_lck_grp, "apfs-check-walkers");
static LCK_MTX_DECLARE(par_lock, &par_lck_grp);
static struct par_dir *par_stack;
static uint32_t par_count, par_cap, par_busy, par_finished, par_most;
static bool par_failed;

static void
par_push(const char *path, size_t len, int depth)
{
	char *copy = kalloc_data(len + 1, Z_WAITOK);

	lck_mtx_lock(&par_lock);
	if (par_count == par_cap) {
		uint32_t cap = par_cap ? par_cap * 2 : 1024;
		struct par_dir *grown = kalloc_data(cap * sizeof(*grown), Z_WAITOK);

		if (grown == NULL) {
			par_failed = true;
			lck_mtx_unlock(&par_lock);
			kfree_data(copy, len + 1);
			return;
		}
		if (par_stack != NULL) {
			memcpy(grown, par_stack, par_count * sizeof(*grown));
			kfree_data(par_stack, par_cap * sizeof(*grown));
		}
		par_stack = grown;
		par_cap = cap;
	}
	if (copy == NULL) {
		par_failed = true;
	} else {
		memcpy(copy, path, len + 1);
		par_stack[par_count++] = (struct par_dir){ copy, len, depth };
		if (par_count > par_most) {
			par_most = par_count;
		}
		wakeup(&par_stack);
	}
	lck_mtx_unlock(&par_lock);
}

static void
par_walker(void *arg, __unused wait_result_t wr)
{
	struct census *cs = arg;
	uint64_t cpu0 = thread_get_runtime_self(), wall0 = mach_absolute_time();

	for (;;) {
		struct par_dir d;
		uint64_t t0 = mach_absolute_time();

		lck_mtx_lock(&par_lock);
		while (par_count == 0 && par_busy > 0) {
			msleep(&par_stack, &par_lock, PRIBIO, "apfs-walkers", NULL);
		}
		cs->t_idle += mach_absolute_time() - t0;
		if (par_count == 0) {
			/* nothing left, and no walker that could leave more */
			cs->t_cpu = thread_get_runtime_self() - cpu0;
			cs->t_wall = mach_absolute_time() - wall0;
			par_finished++;
			wakeup(&par_stack);
			lck_mtx_unlock(&par_lock);
			return;
		}
		d = par_stack[--par_count];
		par_busy++;
		lck_mtx_unlock(&par_lock);

		memcpy(cs->path, d.path, d.len + 1);
		cs->len = d.len;
		cs->depth = d.depth;
		kfree_data(d.path, d.len + 1);
		if (!cs->stop) {
			census_dir(cs);
		}

		lck_mtx_lock(&par_lock);
		par_busy--;
		wakeup(&par_stack);
		lck_mtx_unlock(&par_lock);
	}
}

/* where a walker's (or, summed, all walkers') time went */
static void
census_times(const struct census *cs, int who)
{
	uint64_t v[8] = { cs->t_wall, cs->t_cpu, cs->t_lookup, cs->t_attr, cs->t_read, cs->hash_ticks, cs->t_readdir,
		              cs->t_idle };

	for (int i = 0; i < 8; i++) {
		absolutetime_to_nanoseconds(v[i], &v[i]);
		v[i] /= 1000000;
	}
	char name[24];

	if (who < 0) {
		strlcpy(name, "all walkers", sizeof(name));
	} else {
		snprintf(name, sizeof(name), "walker %d", who);
	}
	printf("apfs-check: %s: %llu entries, %llu bytes: %llu ms, on a processor %llu; lookups %llu, getattr %llu, "
	    "reading %llu (of it hashing %llu), readdir %llu, waiting for a directory %llu\n",
	    name, cs->entries, cs->bytes, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/* The whole walk on n threads, its totals into cs */
static void
par_walk(struct census *cs, uint32_t n)
{
	struct census *w[PAR_MAX];
	uint32_t started = 0;

	par_count = par_busy = par_finished = par_most = 0;
	par_failed = false;
	par_push(cs->path, cs->len, cs->depth);
	for (uint32_t i = 0; i < n; i++) {
		w[i] = kalloc_type(struct census, Z_WAITOK | Z_ZERO);
		if (w[i] == NULL) {
			break;
		}
		*w[i] = *cs;
		w[i]->par = true;
		w[i]->sum = w[i]->entries = w[i]->dirs = w[i]->files = w[i]->links = w[i]->others = w[i]->bytes = 0;
		w[i]->errors = 0;
		w[i]->hash_ticks = 0;
		w[i]->path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
		w[i]->buf = kalloc_data(READ_CHUNK, Z_WAITOK);
		if (w[i]->path == NULL || w[i]->buf == NULL) {
			break;
		}
		par_cs[i] = w[i];
		started = i + 1;
	}
	par_n = started;
	printf("apfs-check: %u walkers at once\n", started);
	for (uint32_t i = 0; i < started; i++) {
		thread_t th;

		if (kernel_thread_start(par_walker, w[i], &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		} else {
			lck_mtx_lock(&par_lock);
			par_finished++;
			lck_mtx_unlock(&par_lock);
		}
	}
	lck_mtx_lock(&par_lock);
	while (par_finished < started) {
		msleep(&par_stack, &par_lock, PRIBIO, "apfs-walk", NULL);
	}
	lck_mtx_unlock(&par_lock);
	par_n = 0;
	for (uint32_t i = 0; i < started; i++) {
		par_cs[i] = NULL;
		cs->sum += w[i]->sum;
		cs->entries += w[i]->entries;
		cs->dirs += w[i]->dirs;
		cs->files += w[i]->files;
		cs->links += w[i]->links;
		cs->others += w[i]->others;
		cs->bytes += w[i]->bytes;
		cs->errors += w[i]->errors;
		cs->hash_ticks += w[i]->hash_ticks;
		census_times(w[i], i);
		cs->t_lookup += w[i]->t_lookup;
		cs->t_attr += w[i]->t_attr;
		cs->t_read += w[i]->t_read;
		cs->t_readdir += w[i]->t_readdir;
		cs->t_idle += w[i]->t_idle;
		cs->t_cpu += w[i]->t_cpu;
		cs->t_wall += w[i]->t_wall;
		kfree_data(w[i]->path, PATH_CAP);
		kfree_data(w[i]->buf, READ_CHUNK);
		kfree_type(struct census, w[i]);
	}
	if (par_failed) {
		printf("apfs-check: the walkers ran out of memory for their stack: the walk is NOT whole\n");
		cs->errors++;
	}
	census_times(cs, -1);
	printf("apfs-check: at most %u directories waited on the walkers' stack\n", par_most);
	if (par_stack != NULL) {
		kfree_data(par_stack, par_cap * sizeof(*par_stack));
		par_stack = NULL;
		par_cap = 0;
	}
}

/* ---- getting there ---- */

static int
make_mount_point(vfs_context_t ctx)
{
	struct componentname cn;
	struct vnode_attr va;
	vnode_t dvp = NULLVP, vp = NULLVP;
	char leaf[] = CHECK_LEAF;
	int error;

	error = vnode_lookup(CHECK_PARENT, 0, &dvp, ctx);
	if (error) {
		return error;
	}
	bzero(&cn, sizeof(cn));
	cn.cn_nameiop = CREATE;
	cn.cn_flags = ISLASTCN;
	cn.cn_context = ctx;
	cn.cn_pnbuf = leaf;
	cn.cn_pnlen = (int)sizeof(leaf);
	cn.cn_nameptr = leaf;
	cn.cn_namelen = (int)(sizeof(leaf) - 1);

	VATTR_INIT(&va);
	VATTR_SET(&va, va_type, VDIR);
	VATTR_SET(&va, va_mode, 0755);

	error = VNOP_MKDIR(dvp, &vp, &cn, &va, ctx);
	if (error == 0 && vp != NULLVP) {
		vnode_put(vp);
	}
	vnode_put(dvp);
	if (error == EEXIST) {
		error = 0;
	}
	return error;
}

#define MAX_THREADS 8

static volatile int apfs_check_running;

struct walk_job {
	int      index;
	uint64_t ms;
	uint64_t entries;
	uint64_t bytes;
	uint64_t sum;
	uint32_t errors;
};

static struct walk_job apfs_jobs[MAX_THREADS];

/* One reader: the whole root filesystem, with everything of its own. */
static void
apfs_walk_one(void *arg, __unused wait_result_t wr)
{
	struct walk_job *job = arg;
	struct census cs;
	uint64_t start, end, ns = 0, hash_ns = 0;

	bzero(&cs, sizeof(cs));
	cs.ctx = vfs_context_kernel();
	census_options(&cs);
	PE_parse_boot_argn("apfs_depth", &cs.max_depth, sizeof(cs.max_depth));
	PE_parse_boot_argn("apfs_max", &cs.max_entries, sizeof(cs.max_entries));
	{
		static char skip[256];

		if (PE_parse_boot_arg_str("apfs_skip", skip, sizeof(skip)) && skip[0] != '\0') {
			cs.skip = skip;
			printf("apfs-check: staying out of %s\n", skip);
		}
	}
	cs.lookup_flags = VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT;
	cs.path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
	cs.buf = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (cs.path == NULL || cs.buf == NULL) {
		printf("apfs-check: reader %d: out of memory\n", job->index);
		OSDecrementAtomic(&apfs_check_running);
		return;
	}
	start = mach_absolute_time();
	census_dir(&cs);
	end = mach_absolute_time();
	absolutetime_to_nanoseconds(end - start, &ns);
	absolutetime_to_nanoseconds(cs.hash_ticks, &hash_ns);

	job->ms = ns / 1000000;
	job->entries = cs.entries;
	job->bytes = cs.bytes;
	job->sum = cs.sum;
	job->errors = cs.errors;
	printf("apfs-check: reader %d: %llu entries  %llu bytes  sum %016llx  errors %u  "
	    "%llu ms  hash %s%llu ms\n", job->index, cs.entries, cs.bytes,
	    (unsigned long long)cs.sum, cs.errors, job->ms, cs.no_hash ? "off " : "",
	    hash_ns / 1000000);

	kfree_data(cs.path, PATH_CAP);
	kfree_data(cs.buf, READ_CHUNK);
	OSDecrementAtomic(&apfs_check_running);
}

/*
 * Several readers at once over the same volume.  What is wanted is not only that nothing
 * breaks but what the lock costs: all of apfs_lib runs under one mutex, so the part of the
 * work that is held time cannot go faster however many processors there are.
 */
static void
apfs_check_concurrent(void)
{
	uint32_t want = 4;
	uint64_t start, end, ns = 0;
	uint64_t total_bytes = 0, total_entries = 0;
	int i, n, bad = 0;

	if (PE_parse_boot_argn("apfs_threads", &want, sizeof(want)) == 0 || want == 0) {
		want = 4;
	}
	if (want > MAX_THREADS) {
		want = MAX_THREADS;
	}
	n = (int)want;
	printf("apfs-check: %d readers over the root filesystem at once\n", n);

	bzero(apfs_jobs, sizeof(apfs_jobs));
	apfs_check_running = n;
	start = mach_absolute_time();
	for (i = 0; i < n; i++) {
		thread_t th;

		apfs_jobs[i].index = i;
		if (kernel_thread_start(apfs_walk_one, &apfs_jobs[i], &th) != KERN_SUCCESS) {
			printf("apfs-check: cannot start reader %d\n", i);
			OSDecrementAtomic(&apfs_check_running);
			continue;
		}
		thread_deallocate(th);
	}
	while (apfs_check_running > 0) {
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	end = mach_absolute_time();
	absolutetime_to_nanoseconds(end - start, &ns);

	for (i = 0; i < n; i++) {
		total_bytes += apfs_jobs[i].bytes;
		total_entries += apfs_jobs[i].entries;
		if (apfs_jobs[i].sum != apfs_jobs[0].sum ||
		    apfs_jobs[i].entries != apfs_jobs[0].entries || apfs_jobs[i].errors != 0) {
			bad++;
		}
	}
	printf("apfs-check: %d readers done in %llu ms\n", n, ns / 1000000);
	printf("apfs-check: together %llu entries  %llu bytes  %llu KB/s\n", total_entries,
	    total_bytes, ns ? (total_bytes * 1000000ull) / ns : 0);
	printf("apfs-check: readers that disagreed or failed: %d\n", bad);
	apfs_stats_print();
}

/* ------------------------------------------- apfs_check=7: the kernel's own write, on a device */

/*
 * The one place bsd/miscfs/apfs/apfs_write.c is run inside the kernel.  It is not VFS: there is
 * no vnop_write and this milestone does not call for one.  It is apfs_lib and apfs_write on a
 * real block device, which is the step between the tests on the build machine
 * (tools/apfs/write-tests.sh) and anything on the phone's flash.
 *
 *   apfs_check=7
 *   apfs_dev=md0              the container to write in (default rvblk0)
 *   apfs_write_path=/redwood/x   the file in it, by path from the volume's root
 *   apfs_write_at=N           where in the file (default 0)
 *   apfs_write_len=N          how many bytes (default 4096, at most APFS_WRITE_MAX)
 *   apfs_write_byte=N         what to fill them with (default 0xa5)
 *
 * The container is opened, written and closed; then opened again from nothing and the file read
 * back, so what is reported came off the device and not out of apfs_lib's block cache.  The
 * device is opened for writing here and nowhere else: every mount of this driver leaves
 * dev.write NULL, and without it apfs_write_in_place refuses everything.
 *
 * Where this can reach, which is the point: a block device.  The phone's flash is not one under
 * this kernel -- it is read through the loader's mailbox and, on the branch that writes to it at
 * all, through a character device of its own with its own bounds (bsd/dev/redwood_ufs.c) -- so
 * nothing here can touch it.  In QEMU and on the phone alike the only block devices are RAM
 * disks.  And it happens at all only when asked for twice over: apfs_check=7 and a path.
 */
#define APFS_WRITE_MAX (1u << 20)

struct wdev {
	vnode_t  vp;
	uint32_t devblock;
};

static int
wdev_io(struct wdev *w, uint64_t off, void *buf, size_t len, int flag)
{
	char *p = buf;

	if ((off % w->devblock) != 0 || (len % w->devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < (128u << 10) ? len : (128u << 10);
		buf_t bp = buf_alloc(w->vp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, flag);
		buf_setblkno(bp, (daddr64_t)(off / w->devblock));
		buf_setlblkno(bp, (daddr64_t)(off / w->devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)p);
		/*
		 * A write buffer of our own has to be counted against the device's vnode before
		 * it is issued: buf_biodone() undoes that count, and without it the kernel
		 * panics on the way back (vnode_writedone: numoutput < 0).  This is what
		 * cluster_io() does for the same kind of raw buffer.
		 */
		if (flag != B_READ) {
			vnode_startwrite(w->vp);
		}
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int
wdev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return wdev_io(ctx, off, buf, len, B_READ);
}

static int
wdev_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	return wdev_io(ctx, off, (void *)(uintptr_t)buf, len, B_WRITE);
}

/* The volume a path names, and the inode in it; the first volume with a root directory. */
static int
wdev_open_volume(struct apfs_nx *nx, struct apfs_vol *vol)
{
	uint32_t i;

	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_inode ino;

		if (nx->fs_oid[i] == 0 || apfs_vol_open(nx, i, vol) != 0) {
			continue;
		}
		if (apfs_inode_get(vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
			return 0;
		}
	}
	return ENOENT;
}

/* The FNV-1a of a file's contents as the census lines use it, read through apfs_lib. */
static int
wdev_hash(struct apfs_vol *vol, const struct apfs_inode *ino, uint8_t *buf, size_t buflen,
    uint64_t *out)
{
	struct apfs_cfile cf;
	uint64_t h = FNV_INIT, off = 0;

	bzero(&cf, sizeof(cf));
	while (off < ino->size) {
		size_t want = (size_t)(ino->size - off), got = 0, i;

		if (want > buflen) {
			want = buflen;
		}
		if (apfs_read_file(vol, ino, &cf, off, buf, want, &got) != 0 || got == 0) {
			apfs_cfile_fini(&cf);
			return EIO;
		}
		for (i = 0; i < got; i++) {
			h ^= buf[i];
			h *= FNV_PRIME;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	*out = h;
	return 0;
}

static void
apfs_write_check(const char *devname)
{
	vfs_context_t ctx = vfs_context_kernel();
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_write_report rep;
	struct apfs_dev dev;
	struct wdev w = { NULLVP, 0 };
	char path[256] = "";
	uint8_t *data = NULL, *scratch = NULL, *was = NULL;
	uint64_t at = 0, before = 0, after = 0, want = 0, file_id = 0, was_len = 0;
	uint32_t len = 4096, byte = 0xa5;
	const char *why = NULL;
	bool opened = false, nx_open = false;
	int error, tries;

	if (!PE_parse_boot_arg_str("apfs_write_path", path, sizeof(path)) || path[0] != '/') {
		printf("apfs-write: apfs_write_path=/some/file says which file to write\n");
		return;
	}
	PE_parse_boot_argn("apfs_write_at", &at, sizeof(at));
	PE_parse_boot_argn("apfs_write_len", &len, sizeof(len));
	PE_parse_boot_argn("apfs_write_byte", &byte, sizeof(byte));
	if (len == 0 || len > APFS_WRITE_MAX) {
		printf("apfs-write: apfs_write_len=%u is not between 1 and %u\n", len, APFS_WRITE_MAX);
		return;
	}

	for (tries = 0; tries < 100; tries++) {
		if (vnode_lookup(devname, 0, &w.vp, ctx) == 0) {
			break;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (w.vp == NULLVP) {
		printf("apfs-write: %s never showed up in /dev\n", devname);
		return;
	}
	/* for writing, which is what no mount of this driver ever asks for */
	error = VNOP_OPEN(w.vp, FREAD | FWRITE, ctx);
	if (error) {
		printf("apfs-write: %s will not open for writing: %d\n", devname, error);
		goto out;
	}
	opened = true;
	if (VNOP_IOCTL(w.vp, DKIOCGETBLOCKSIZE, (caddr_t)&w.devblock, 0, ctx) != 0 ||
	    w.devblock == 0 || (4096 % w.devblock) != 0) {
		printf("apfs-write: %s has no usable block size\n", devname);
		goto out;
	}
	data = kalloc_data(len, Z_WAITOK);
	scratch = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (data == NULL || scratch == NULL) {
		printf("apfs-write: out of memory\n");
		goto out;
	}
	memset(data, (int)(byte & 0xff), len);

	memset(&dev, 0, sizeof(dev));
	dev.ctx = &w;
	dev.read = wdev_read;
	dev.read_data = wdev_read;
	dev.write = wdev_write;
	error = apfs_nx_open(&nx, &dev);
	if (error) {
		printf("apfs-write: %s is not a container we can read: %d\n", devname, error);
		goto out;
	}
	nx_open = true;
	error = wdev_open_volume(&nx, &vol);
	if (error) {
		printf("apfs-write: %s has no volume with a root directory\n", devname);
		goto out;
	}
	if (apfs_write_volume_ok(&vol, &why) != 0) {
		printf("apfs-write: volume %s will not be written: %s\n", vol.name, why);
		goto out;
	}
	error = apfs_path_lookup(&vol, path, &file_id, NULL);
	if (error) {
		printf("apfs-write: %s: no such path in %s: %d\n", path, vol.name, error);
		goto out;
	}
	error = apfs_inode_get(&vol, file_id, &ino);
	if (error == 0) {
		error = wdev_hash(&vol, &ino, scratch, READ_CHUNK, &before);
	}
	if (error) {
		printf("apfs-write: %s cannot be read before the write: %d\n", path, error);
		goto out;
	}
	printf("apfs-write: %s on %s, inode %llu, %llu bytes, hash %016llx\n", path, vol.name,
	    file_id, ino.size, before);
	/*
	 * The file as it is now, kept so that what it should hold afterwards can be worked out
	 * from it rather than from the result -- which would prove nothing.
	 */
	if (ino.size <= APFS_WRITE_MAX) {
		struct apfs_cfile cf;
		size_t got = 0;

		bzero(&cf, sizeof(cf));
		was_len = ino.size;
		was = kalloc_data((vm_size_t)was_len, Z_WAITOK);
		if (was != NULL &&
		    (apfs_read_file(&vol, &ino, &cf, 0, was, (size_t)was_len, &got) != 0 ||
		    got != was_len)) {
			kfree_data(was, (vm_size_t)was_len);
			was = NULL;
		}
		apfs_cfile_fini(&cf);
	}
	if (was == NULL) {
		printf("apfs-write: %s is too big to keep a copy of: only the hashes are reported\n",
		    path);
	}

	error = apfs_write_in_place(&vol, file_id, at, data, len, &rep);
	printf("apfs-write: %u bytes of 0x%02x at %llu: %s; %llu blocks from %llu, %llu of them "
	    "patched whole, %llu writes\n", len, byte & 0xff, at,
	    error == 0 ? "written" : (rep.why != NULL ? rep.why : "failed"), rep.blocks,
	    rep.first_block, rep.partial, rep.calls);
	apfs_nx_close(&nx);
	nx_open = false;
	if (error) {
		goto out;
	}

	/*
	 * Again from nothing: a container opened a second time shares no cache with the first,
	 * so what comes back now is what is on the device.
	 */
	error = apfs_nx_open(&nx, &dev);
	if (error == 0) {
		nx_open = true;
		error = wdev_open_volume(&nx, &vol);
	}
	if (error == 0) {
		error = apfs_path_lookup(&vol, path, &file_id, NULL);
	}
	if (error == 0) {
		error = apfs_inode_get(&vol, file_id, &ino);
	}
	if (error == 0) {
		error = wdev_hash(&vol, &ino, scratch, READ_CHUNK, &after);
	}
	if (error) {
		printf("apfs-write: the volume will not read back after the write: %d\n", error);
		goto out;
	}
	if (was != NULL) {
		uint64_t off;

		want = FNV_INIT;
		for (off = 0; off < was_len; off++) {
			uint8_t b = (off >= at && off < at + len) ? (uint8_t)(byte & 0xff) : was[off];

			want ^= b;
			want *= FNV_PRIME;
		}
	}
	printf("apfs-write: read back from a container opened again: %llu bytes, hash %016llx "
	    "(was %016llx); the bytes written are %s\n", ino.size, after, before,
	    was == NULL ? "not checked here" :
	    (want == after ? "all there and nothing else changed" : "NOT what was asked for"));
out:
	if (nx_open) {
		apfs_nx_close(&nx);
	}
	if (data != NULL) {
		kfree_data(data, len);
	}
	if (scratch != NULL) {
		kfree_data(scratch, READ_CHUNK);
	}
	if (was != NULL) {
		kfree_data(was, (vm_size_t)was_len);
	}
	if (opened) {
		(void)VNOP_CLOSE(w.vp, FREAD | FWRITE, ctx);
	}
	if (w.vp != NULLVP) {
		vnode_put(w.vp);
	}
}

/* -------------------------------------- apfs_check=8: the store, in the kernel, on a device */

/*
 * The store of fixed-length slot files (bsd/miscfs/apfs/apfs_store.c) run where it will live: in
 * the kernel, on a block device, under the kernel's own allocator.  A payload of apfs_store_len
 * bytes of a pattern this works out twice -- once to save, once to compare -- goes into the
 * store on apfs_dev; then the container is opened again from nothing and the newest good slot is
 * read back, so what is reported came off the device.
 *
 *   apfs_check=8
 *   apfs_dev=md1              the store volume (default rvblk0); its name must be RedwoodStore
 *   apfs_store_len=N          how many bytes of payload (default 40000)
 *   apfs_store_seed=N         which pattern (default 1); a different seed is a different payload
 *   apfs_store_note=N         the writer's own number, kept in the header (default 0)
 */
#define APFS_STORE_TEST_MAX (4u << 20)

static void
store_pattern(uint8_t *buf, uint32_t len, uint32_t seed)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		buf[i] = (uint8_t)(i * (7 + seed) + 5 + seed);
	}
}

static void
apfs_store_check(const char *devname)
{
	vfs_context_t ctx = vfs_context_kernel();
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_head head;
	struct apfs_store_write w;
	struct apfs_dev dev;
	struct wdev wd = { NULLVP, 0 };
	uint8_t *data = NULL, *scratch = NULL;
	uint32_t len = 40000, seed = 1;
	uint64_t note = 0, want = FNV_INIT;
	const char *why = NULL;
	bool opened = false, nx_open = false;
	int best, error, tries;
	uint32_t i;

	PE_parse_boot_argn("apfs_store_len", &len, sizeof(len));
	PE_parse_boot_argn("apfs_store_seed", &seed, sizeof(seed));
	PE_parse_boot_argn("apfs_store_note", &note, sizeof(note));
	if (len == 0 || len > APFS_STORE_TEST_MAX) {
		printf("apfs-store: apfs_store_len=%u is not between 1 and %u\n", len,
		    APFS_STORE_TEST_MAX);
		return;
	}
	for (tries = 0; tries < 100; tries++) {
		if (vnode_lookup(devname, 0, &wd.vp, ctx) == 0) {
			break;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (wd.vp == NULLVP) {
		printf("apfs-store: %s never showed up in /dev\n", devname);
		return;
	}
	error = VNOP_OPEN(wd.vp, FREAD | FWRITE, ctx);
	if (error) {
		printf("apfs-store: %s will not open for writing: %d\n", devname, error);
		goto out;
	}
	opened = true;
	if (VNOP_IOCTL(wd.vp, DKIOCGETBLOCKSIZE, (caddr_t)&wd.devblock, 0, ctx) != 0 ||
	    wd.devblock == 0 || (4096 % wd.devblock) != 0) {
		printf("apfs-store: %s has no usable block size\n", devname);
		goto out;
	}
	data = kalloc_data(len, Z_WAITOK);
	scratch = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (data == NULL || scratch == NULL) {
		printf("apfs-store: out of memory\n");
		goto out;
	}
	store_pattern(data, len, seed);
	for (i = 0; i < len; i++) {
		want ^= data[i];
		want *= FNV_PRIME;
	}

	memset(&dev, 0, sizeof(dev));
	dev.ctx = &wd;
	dev.read = wdev_read;
	dev.read_data = wdev_read;
	dev.write = wdev_write;
	error = apfs_nx_open(&nx, &dev);
	if (error) {
		printf("apfs-store: %s is not a container we can read: %d\n", devname, error);
		goto out;
	}
	nx_open = true;
	error = wdev_open_volume(&nx, &vol);
	if (error == 0) {
		error = apfs_store_open(&vol, &st, &why);
	}
	if (error) {
		printf("apfs-store: %s holds no store: %s\n", devname,
		    why != NULL ? why : "cannot be opened");
		goto out;
	}
	best = apfs_store_newest(&st, scratch, READ_CHUNK, &head);
	printf("apfs-store: %s: %d slots of %llu bytes, %llu of payload; the newest good slot is "
	    "%d%s\n", vol.name, (int)APFS_STORE_SLOTS, st.slot_bytes, st.payload_max, best,
	    best < 0 ? " (none)" : "");

	error = apfs_store_save(&st, data, len, 0, note, scratch, READ_CHUNK, &w);
	printf("apfs-store: %u bytes into slot %d as generation %llu: %s; %llu blocks\n", len,
	    w.slot, w.generation, error == 0 ? "saved" : "failed", w.blocks);
	apfs_nx_close(&nx);
	nx_open = false;
	if (error) {
		printf("apfs-store: the save failed: %d\n", error);
		goto out;
	}

	/* again from nothing: no cache of the first opening survives into the second */
	error = apfs_nx_open(&nx, &dev);
	if (error == 0) {
		nx_open = true;
		error = wdev_open_volume(&nx, &vol);
	}
	if (error == 0) {
		error = apfs_store_open(&vol, &st, &why);
	}
	if (error) {
		printf("apfs-store: the store will not open again: %d\n", error);
		goto out;
	}
	best = apfs_store_newest(&st, scratch, READ_CHUNK, &head);
	if (best < 0) {
		printf("apfs-store: after the save no slot holds anything good\n");
		goto out;
	}
	printf("apfs-store: read back from a container opened again: slot %d, generation %llu, "
	    "%llu bytes, note %llu, sum %016llx; the payload is %s\n", best, head.generation,
	    head.bytes, head.note, head.sum,
	    (head.bytes == len && head.sum == want) ? "the one that was saved" :
	    "NOT what was saved");
out:
	if (nx_open) {
		apfs_nx_close(&nx);
	}
	if (data != NULL) {
		kfree_data(data, len);
	}
	if (scratch != NULL) {
		kfree_data(scratch, READ_CHUNK);
	}
	if (opened) {
		(void)VNOP_CLOSE(wd.vp, FREAD | FWRITE, ctx);
	}
	if (wd.vp != NULLVP) {
		vnode_put(wd.vp);
	}
}

static void
apfs_check_thread(void *arg, __unused wait_result_t wr)
{
	uint32_t mode = (uint32_t)(uintptr_t)arg;
	vfs_context_t ctx = vfs_context_kernel();
	struct census cs;
	char devname[64] = CHECK_DEV;
	char subtree[256] = "";
	uint64_t start, end;
	int error, tries;

	bzero(&cs, sizeof(cs));
	cs.ctx = ctx;
	census_options(&cs);
	cs.print_lines = (mode == 2);
	/*
	 * NOCROSSMOUNT only where the volume is the root (modes 4 and 5).  In mode 6 the
	 * path leads from the root image into the mount, so crossing has to be allowed; the
	 * walk is kept on the volume by the check against the mount it started on.
	 */
	PE_parse_boot_argn("apfs_depth", &cs.max_depth, sizeof(cs.max_depth));
	PE_parse_boot_argn("apfs_max", &cs.max_entries, sizeof(cs.max_entries));
	{
		static char skip[256];

		if (PE_parse_boot_arg_str("apfs_skip", skip, sizeof(skip)) && skip[0] != '\0') {
			cs.skip = skip;
			printf("apfs-check: staying out of %s\n", skip);
		}
	}
	cs.lookup_flags = VNODE_LOOKUP_NOFOLLOW |
	    ((mode == 4 || mode == 5) ? VNODE_LOOKUP_NOCROSSMOUNT : 0);
	if (PE_parse_boot_arg_str("apfs_dev", subtree, sizeof(subtree)) && subtree[0] != '\0') {
		snprintf(devname, sizeof(devname), "/dev/%s", subtree);
	}
	subtree[0] = '\0';

	/* the modes that write: no walk, no mount, no census */
	if (mode == 7) {
		apfs_write_check(devname);
		return;
	}
	if (mode == 8) {
		apfs_store_check(devname);
		return;
	}

	cs.path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
	cs.buf = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (cs.path == NULL || cs.buf == NULL) {
		printf("apfs-check: out of memory\n");
		return;
	}

	if (mode == 6) {
		char mnt[128] = CHECK_MNT;

		if (PE_parse_boot_arg_str("apfs_mnt", subtree, sizeof(subtree)) &&
		    subtree[0] == '/') {
			strlcpy(mnt, subtree, sizeof(mnt));
		}
		subtree[0] = '\0';
		/*
		 * A node just made with devfs_make_node() is not in /dev the instant it is
		 * made -- this runs straight after the device registered itself, and the first
		 * lookup comes back ENOENT -- so wait for it rather than guess how long devfs
		 * takes to show it.
		 */
		{
			vnode_t t = NULLVP;
			int tries;

			for (tries = 0; tries < 100; tries++) {
				if (vnode_lookup(devname, 0, &t, ctx) == 0) {
					vnode_put(t);
					break;
				}
				delay_for_interval(20, NSEC_PER_MSEC);
			}
			if (tries >= 100) {
				printf("apfs-check: %s never showed up in /dev\n", devname);
			} else if (tries > 0) {
				printf("apfs-check: %s showed up in /dev after %d ms\n", devname,
				    tries * 20);
			}
		}
		error = kernel_mount("apfs", NULLVP, NULLVP, mnt, devname, 0,
		    MNT_RDONLY | MNT_DONTBROWSE, KERNEL_MOUNT_NOAUTH, ctx);
		if (error) {
			printf("apfs-check: mounting %s on %s failed: %d\n", devname, mnt,
			    error);
			kfree_data(cs.path, PATH_CAP);
			kfree_data(cs.buf, READ_CHUNK);
			return;
		}
		printf("apfs-check: %s mounted on %s\n", devname, mnt);
		cs.rel = strlen(mnt);
		strlcpy(cs.path, mnt, PATH_CAP);
		cs.len = cs.rel;
		if (PE_parse_boot_arg_str("apfs_subtree", subtree, sizeof(subtree)) &&
		    subtree[0] == '/') {
			strlcat(cs.path, subtree, PATH_CAP);
			cs.len = strlen(cs.path);
			printf("apfs-check: walking %s\n", cs.path + cs.rel);
		} else {
			printf("apfs-check: walking the whole volume\n");
		}
		goto walk;
	}
	if (mode >= 5) {
		kfree_data(cs.path, PATH_CAP);
		kfree_data(cs.buf, READ_CHUNK);
		apfs_check_concurrent();
		return;
	}
	if (mode >= 4 && mode != 6) {
		/*
		 * The volume is already the root: nothing to mount, and no mount point to
		 * make, so none of what follows about /private/var applies.
		 */
		cs.rel = 0;
		cs.len = 0;
		cs.path[0] = '\0';
		printf("apfs-check: walking the root filesystem\n");
		goto walk;
	}

	/*
	 * /private/var is a ramfs that userland's init mounts, so the mount point cannot be
	 * made until that has happened.  Wait for it rather than guess how long it takes.
	 */
	for (tries = 0; tries < 180; tries++) {
		error = make_mount_point(ctx);
		if (error == 0) {
			break;
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	if (error) {
		printf("apfs-check: cannot make %s: %d\n", CHECK_MNT, error);
		return;
	}

	/*
	 * The mount data is the device's path itself, not a pointer to it: the extra
	 * indirection imageboot_pivot_image() uses is unpacked only on the BaseSystem path,
	 * which this build does not have, so apfs_mount() reads the string straight off
	 * this pointer.
	 */
	error = kernel_mount("apfs", NULLVP, NULLVP, CHECK_MNT, devname, 0,
	    MNT_RDONLY | MNT_DONTBROWSE, KERNEL_MOUNT_NOAUTH, ctx);
	if (error) {
		printf("apfs-check: mounting %s on %s failed: %d\n", devname, CHECK_MNT,
		    error);
		return;
	}
	printf("apfs-check: %s mounted on %s\n", devname, CHECK_MNT);
	if (mode >= 3) {
		/* someone in user space is going to read it; leave it alone */
		printf("apfs-check: left mounted, not walking it here\n");
		return;
	}

	cs.rel = sizeof(CHECK_MNT) - 1;
	strlcpy(cs.path, CHECK_MNT, PATH_CAP);
	cs.len = cs.rel;
	if (PE_parse_boot_arg_str("apfs_subtree", subtree, sizeof(subtree)) &&
	    subtree[0] == '/') {
		strlcat(cs.path, subtree, PATH_CAP);
		cs.len = strlen(cs.path);
		while (cs.len > cs.rel && cs.path[cs.len - 1] == '/') {
			cs.path[--cs.len] = '\0';
		}
		printf("apfs-check: walking %s\n", cs.path + cs.rel);
	} else {
		printf("apfs-check: walking the whole volume\n");
	}

walk:
	redwood_ufs_report("before the walk");
	start = mach_absolute_time();
	cs.t0 = start;
	watch_start(&cs);
	{
		uint32_t walkers = 1;

		PE_parse_boot_argn("apfs_walkers", &walkers, sizeof(walkers));
		if (mode == 6 && walkers > 1) {
			par_walk(&cs, walkers > PAR_MAX ? PAR_MAX : walkers);
		} else {
			uint64_t cpu0 = thread_get_runtime_self(), wall0 = mach_absolute_time();

			census_dir(&cs);
			cs.t_cpu = thread_get_runtime_self() - cpu0;
			cs.t_wall = mach_absolute_time() - wall0;
			census_times(&cs, 0);
		}
	}
	watch_stop();
	end = mach_absolute_time();

	{
		uint64_t ns = 0;

		absolutetime_to_nanoseconds(end - start, &ns);
		printf("apfs-check: done in %llu ms\n", ns / 1000000);
	}
	printf("apfs-check: entries %llu  dirs %llu  files %llu  symlinks %llu  other %llu\n",
	    cs.entries, cs.dirs, cs.files, cs.links, cs.others);
	printf("apfs-check: content bytes %llu\n", cs.bytes);
	printf("apfs-check: line-hash sum %016llx\n", (unsigned long long)cs.sum);
	printf("apfs-check: errors %u\n", cs.errors);
	{
		uint64_t hash_ns = 0;

		absolutetime_to_nanoseconds(cs.hash_ticks, &hash_ns);
		printf("apfs-check: of it the checker's own hashing %llu ms%s\n", hash_ns / 1000000,
		    cs.no_hash ? " (apfs_hash=0)" : "");
	}
	apfs_stats_print();
	redwood_sparse_report();
	redwood_ufs_report("after the walk");
	printf("apfs-check: compare with tools/apfs/verify.py sum\n");

	kfree_data(cs.path, PATH_CAP);
	kfree_data(cs.buf, READ_CHUNK);
}

static volatile int apfs_check_finished;

static void
apfs_check_runner(void *arg, wait_result_t wr)
{
	apfs_check_thread(arg, wr);
	apfs_check_finished = 1;
}

/*
 * sysctl kern.redwood_switch_root="INCOMING OUTGOING": the volume mounted on INCOMING
 * becomes the root, and what was the root until now is mounted on OUTGOING inside it (a
 * path without a leading slash, which must be a directory there -- on the iOS volume, a
 * synthetic one).  devfs stays on /dev (vfs_switch_root() keeps it there).  This is how
 * the iOS root volume on the phone's flash becomes the root: the kernel starts from the
 * small image, whose init mounts the volume, puts in place what the system needs on it
 * and then asks for this (userland/src/init.c, boot-arg root_apfs=).  Process 1 only, once.
 */
static int redwood_root_switched;

static int
redwood_sysctl_switch_root SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[512], *outgoing;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, redwood_root_switched ? "1" : "0", 2);
	}
	if (proc_getpid(current_proc()) != 1 || suser(kauth_cred_get(), NULL) != 0) {
		return EPERM;
	}
	if (redwood_root_switched) {
		return EBUSY;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(buf)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, buf, req->newlen);
	if (error) {
		return error;
	}
	buf[req->newlen] = '\0';
	outgoing = strchr(buf, ' ');
	if (outgoing == NULL || buf[0] != '/') {
		return EINVAL;
	}
	*outgoing++ = '\0';
	error = vfs_switch_root(buf, outgoing, 0);
	printf("redwood: %s is the root now, the one before it on /%s: %d\n", buf, outgoing, error);
	if (error == 0) {
		redwood_root_switched = 1;
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, redwood_switch_root, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, redwood_sysctl_switch_root, "A",
    "write \"INCOMING OUTGOING\": the volume on INCOMING becomes the root (process 1, once)");

/*
 * apfs_volume=DEV: a volume for user space in an ordinary boot.  Mounted read only (and
 * nosuid, nodev) on apfs_volume_at -- by default /System/Volumes/iOS, which the system image
 * has for it (userland/build.sh) -- right here, after the root and devfs and before launchd,
 * so that all of user space sees it from the start.  apfs_volume=ufs_darwin is the iOS root
 * container on the phone's flash (ufs=2 for the loader, bootcpu=7).
 */
static void
apfs_volume_mount(void)
{
	char dev[48], at[256] = "/System/Volumes/iOS", path[64];
	vfs_context_t ctx = vfs_context_kernel();
	uint64_t t0, ns = 0;
	vnode_t vp = NULLVP;
	int error, tries;

	if (!PE_parse_boot_arg_str("apfs_volume", dev, sizeof(dev)) || dev[0] == '\0') {
		return;
	}
	(void)PE_parse_boot_arg_str("apfs_volume_at", at, sizeof(at));
	snprintf(path, sizeof(path), "/dev/%s", dev);
	/* a node devfs_make_node() has just made is not in /dev at once (see apfs_check_thread) */
	for (tries = 0; tries < 100; tries++) {
		if (vnode_lookup(path, 0, &vp, ctx) == 0) {
			vnode_put(vp);
			break;
		}
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	t0 = mach_absolute_time();
	error = kernel_mount("apfs", NULLVP, NULLVP, at, path, 0,
	    MNT_RDONLY | MNT_NOSUID | MNT_NODEV, KERNEL_MOUNT_NOAUTH, ctx);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (error) {
		printf("apfs: mounting %s on %s failed: %d\n", path, at, error);
	} else {
		printf("apfs: %s mounted on %s, read only, in %llu ms\n", path, at, ns / 1000000);
	}
}

void
redwood_apfs_check_init(void)
{
	uint32_t mode = 0;
	thread_t th;

	apfs_volume_mount();

	if (!PE_parse_boot_argn("apfs_check", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (mode >= 4) {
		/*
		 * The volume is the root (or mode 6, which mounts it here), and bsd_init is
		 * about to exec /sbin/launchd off it, which on anything but an image of ours
		 * (stage0 as /sbin/launchd) ends in a panic.  So the walk has to be over before this returns.
		 *
		 * It does not run on this thread, though.  This is the boot thread: it runs
		 * above the threads that service thread calls, and a walk off a RAM disk never
		 * blocks, so for as long as it lasts no thread call runs at all -- not even
		 * the one that feeds the phone's watchdog every ten seconds.  On the phone a
		 * walk past half a minute was reset with no log to say why; in QEMU, which
		 * has no such watchdog, the same walk let not one 6-second timer fire in 72 s.
		 * So the walk gets a thread of ordinary priority, and this one sleeps until
		 * it is done.
		 */
		thread_t th;

		apfs_check_finished = 0;
		if (kernel_thread_start(apfs_check_runner, (void *)(uintptr_t)mode, &th) !=
		    KERN_SUCCESS) {
			printf("apfs-check: cannot start the thread\n");
			return;
		}
		thread_deallocate(th);
		while (!apfs_check_finished) {
			delay_for_interval(100, NSEC_PER_MSEC);     /* sleeps */
		}
		printf("apfs-check: going back to the loader\n");
		loader_return();
		return;
	}
	if (kernel_thread_start(apfs_check_thread, (void *)(uintptr_t)mode, &th) !=
	    KERN_SUCCESS) {
		printf("apfs-check: cannot start the thread\n");
		return;
	}
	thread_deallocate(th);
}
