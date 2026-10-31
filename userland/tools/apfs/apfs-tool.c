/*
 * Run the kernel's APFS reading code on the build machine.
 *
 * bsd/miscfs/apfs/apfs_lib.c is built into this program unchanged, so what it says about
 * an image here is what the driver will see.  `census` prints a line per directory entry
 * that tools/apfs/apfs.py can print too, and run-tests.sh diffs the two: the Python
 * reader has already been checked against 7-Zip over the whole volume, so agreeing with
 * it byte for byte is the C code agreeing with the image.
 *
 * Built by tools/apfs/build-tool.sh.
 *
 * usage: apfs-tool info   IMAGE
 *        apfs-tool ls     IMAGE [PATH]
 *        apfs-tool stat   IMAGE PATH
 *        apfs-tool cat    IMAGE PATH
 *        apfs-tool census IMAGE [PATH] [--meta]   (--meta: do not read contents)
 *        apfs-tool lookups IMAGE    (every name looked up by name; see cmd_lookups)
 *        apfs-tool writable IMAGE   (may the volume be written in place, and why not)
 *        apfs-tool write  IMAGE PATH OFFSET FILE|- [--fail-after N | --corrupt-after N]
 *        apfs-tool store  info IMAGE | save IMAGE FILE|- | load IMAGE FILE|-
 *                                   (the store of fixed-length slot files: bsd/miscfs/apfs/apfs_store.c)
 *                                   (the kernel's apfs_write.c; --fail-after is a power failure,
 *                                    --corrupt-after a device that lies about the write)
 */

/* pread(2) and clock_gettime(2) come with -D_DEFAULT_SOURCE, set by build-tool.sh */
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apfs_lib.h"
#include "apfs_write.h"
#include "apfs_store.h"

/* ---- the image as a device ---- */

static int
file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pread(fd, p, len, (off_t)off);

		if (n <= 0) {
			return EIO;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

/* The same hash the Python side computes, so contents can be compared without the bytes. */
static uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

#define FNV_INIT 0xcbf29ce484222325ULL

/*
 * APFS_TOOL_THREADS=N: nx->parallel on threads of this program, so that what the kernel
 * does on several processors -- whole pieces of one request read and decompressed at once
 * -- runs here the same way (and can be run under ThreadSanitizer).  A thread per worker
 * per call: slow, and only for checking.
 */
struct host_par {
	int (*fn)(void *arg, uint32_t item, uint32_t worker);
	void *arg;
	uint32_t items;
	atomic_uint next;
	atomic_int error;
};

struct host_par_arg {
	struct host_par *hp;
	uint32_t worker;
};

static void
host_par_work(struct host_par *hp, uint32_t worker)
{
	for (;;) {
		uint32_t item = atomic_fetch_add(&hp->next, 1);
		int error, none = 0;

		if (item >= hp->items) {
			break;
		}
		error = hp->fn(hp->arg, item, worker);
		if (error != 0) {
			atomic_compare_exchange_strong(&hp->error, &none, error);
		}
	}
}

static void *
host_par_thread(void *p)
{
	struct host_par_arg *a = p;

	host_par_work(a->hp, a->worker);
	return NULL;
}

static int
host_parallel(void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg)
{
	struct host_par hp;
	struct host_par_arg a[16];
	pthread_t th[16];
	uint32_t w, started = 1;

	(void)ctx;
	hp.fn = fn;
	hp.arg = arg;
	hp.items = items;
	atomic_init(&hp.next, 0);
	atomic_init(&hp.error, 0);
	for (w = 1; w < workers && w < 16; w++) {
		a[w].hp = &hp;
		a[w].worker = w;
		if (pthread_create(&th[w], NULL, host_par_thread, &a[w]) != 0) {
			break;
		}
		started++;
	}
	host_par_work(&hp, 0);
	for (w = 1; w < started; w++) {
		pthread_join(th[w], NULL);
	}
	return atomic_load(&hp.error);
}

static bool want_lock;                 /* the library under a lock, as in the kernel (walkers, cfrace) */
static void host_locking(struct apfs_nx *nx);
static void op_begin(void);
static void op_end(void);

/*
 * The image opened for writing, and the power failure stood in for: after `write_fail_after`
 * blocks the device refuses every write, as a phone whose battery came out mid-write would.
 * Without it nothing refuses; --fail-after 0 refuses the very first block.
 */
static bool want_write;
static bool write_fails;                /* --fail-after was given; 0 of them is a failure too */
static bool write_corrupts;             /* --corrupt-after: a device that lies about a write */
static uint64_t write_fail_after;
static uint64_t writes_done;

static int
file_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	const uint8_t *p = buf;

	uint8_t *copy = NULL;

	if (write_fails && writes_done >= write_fail_after) {
		return EIO;
	}
	if (write_corrupts && writes_done >= write_fail_after) {
		/*
		 * A device that says the write went through and puts something else on the
		 * platter: the one failure no checking before the write can catch, and what the
		 * read-back comparison in apfs_write.c is there for.
		 */
		copy = malloc(len);
		if (copy == NULL) {
			return EIO;
		}
		memcpy(copy, p, len);
		copy[0] ^= 0xff;
		p = copy;
	}
	writes_done++;
	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);

		if (n <= 0) {
			free(copy);
			return EIO;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	free(copy);
	return 0;
}

static int
open_image(const char *path, struct apfs_nx *nx, struct apfs_vol *vol)
{
	struct apfs_dev dev;
	int fd = open(path, want_write ? O_RDWR : O_RDONLY);
	uint32_t i;
	int error;

	if (fd < 0) {
		perror(path);
		return 1;
	}
	memset(&dev, 0, sizeof(dev));
	dev.ctx = (void *)(intptr_t)fd;
	dev.size = (uint64_t)lseek(fd, 0, SEEK_END);
	dev.read = file_read;
	dev.read_data = file_read;      /* pread is as good without a lock as with one */
	if (want_write) {
		dev.write = file_write;
	}

	error = apfs_nx_open(nx, &dev);
	if (error) {
		fprintf(stderr, "apfs-tool: cannot read the container: %s\n", strerror(error));
		return 1;
	}
	if (getenv("APFS_TOOL_THREADS") != NULL) {
		long t = strtol(getenv("APFS_TOOL_THREADS"), NULL, 0);

		if (t > 1) {
			nx->parallel = host_parallel;
			nx->parallel_max = (uint32_t)(t < 16 ? t : 16);
		}
	}
	if (getenv("APFS_TOOL_WALKERS") != NULL && strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0) > 1) {
		want_lock = true;
	}
	if (want_lock) {
		/* handed over now, as the kernel's mount does; from here on every call is with the lock */
		host_locking(nx);
		op_begin();
	}
	/* the first volume that has a root directory, like apfs.py picks it; or APFS_TOOL_VOLUME=N */
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		if (getenv("APFS_TOOL_VOLUME") != NULL && (long)i != strtol(getenv("APFS_TOOL_VOLUME"), NULL, 0)) {
			continue;
		}
		struct apfs_inode ino;

		if (nx->fs_oid[i] == 0) {
			continue;
		}
		if (apfs_vol_open(nx, i, vol) != 0) {
			continue;
		}
		if (apfs_inode_get(vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
			if (want_lock) {
				op_end();
			}
			return 0;
		}
	}
	if (want_lock) {
		op_end();
	}
	fprintf(stderr, "apfs-tool: no readable volume with a root directory\n");
	return 1;
}

/* ---- info ---- */

static int
cmd_info(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	uint32_t i;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	printf("%s: APFS container, block size %u, %" PRIu64 " blocks\n", path,
	    nx.block_size, nx.block_count);
	printf("  newest checkpoint xid %" PRIu64 "\n", nx.xid);
	printf("  container omap block %" PRIu64 "\n", nx.omap_addr);
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_vol v;

		if (nx.fs_oid[i] == 0 || apfs_vol_open(&nx, i, &v) != 0) {
			continue;
		}
		printf("  [%u] %s  role 0x%04x  incompatible 0x%" PRIx64 "  fs flags 0x%" PRIx64 "\n",
		    i, v.name, v.role, v.incompat, v.fs_flags);
		printf("       files %" PRIu64 "  dirs %" PRIu64 "  symlinks %" PRIu64
		    "  blocks used %" PRIu64 "\n", v.num_files, v.num_dirs, v.num_symlinks,
		    v.alloc_count);
		printf("       tree: %" PRIu64 " records in %" PRIu64 " nodes, flags 0x%x\n",
		    v.tree.key_count, v.tree.node_count, v.tree.flags);
	}
	printf("  blocks read %" PRIu64 ", cache hits %" PRIu64 "\n", nx.reads, nx.hits);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- ls ---- */

struct ls_ctx {
	struct apfs_vol *vol;
};

static const char *
mode_kind(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFDIR:  return "d";
	case APFS_S_IFLNK:  return "l";
	case APFS_S_IFREG:  return "-";
	case APFS_S_IFIFO:  return "p";
	case APFS_S_IFCHR:  return "c";
	case APFS_S_IFBLK:  return "b";
	case APFS_S_IFSOCK: return "s";
	default:            return "?";
	}
}

static int
ls_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct ls_ctx *lc = ctx;
	struct apfs_inode ino;

	(void)name_len;
	(void)d_type;
	if (apfs_inode_get(lc->vol, file_id, &ino) != 0) {
		printf("?  %20s  %s  (no inode record)\n", "", name);
		return 0;
	}
	printf("%s %06o %4u:%-4u %12" PRIu64 "  %-20" PRIu64 " %s", mode_kind(ino.mode),
	    ino.mode & 07777, ino.owner, ino.group, ino.size, file_id, name);
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		char target[1024];
		size_t n = 0;

		if (apfs_readlink(lc->vol, file_id, target, sizeof(target) - 1, &n) == 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	printf("\n");
	return 0;
}

static int
cmd_ls(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct ls_ctx lc;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	lc.vol = &vol;
	if (dt != APFS_DT_DIR) {
		ls_cb(&lc, where, strlen(where), id, dt);
	} else {
		error = apfs_readdir(&vol, id, ls_cb, &lc);
		if (error) {
			fprintf(stderr, "apfs-tool: %s\n", strerror(error));
			return 1;
		}
	}
	apfs_nx_close(&nx);
	return 0;
}

/* ---- stat ---- */

static int
xattr_print_cb(void *ctx, const char *name, size_t name_len, const struct apfs_xattr *xa)
{
	(void)ctx;
	(void)name_len;
	printf("  xattr %-28s flags 0x%x  %u bytes%s\n", name, xa->flags, xa->data_len,
	    (xa->flags & APFS_XATTR_DATA_STREAM) ? " (in a data stream)" : "");
	return 0;
}

static int
cmd_stat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	error = apfs_inode_get(&vol, id, &ino);
	if (error) {
		fprintf(stderr, "apfs-tool: inode %" PRIu64 ": %s\n", id, strerror(error));
		return 1;
	}
	printf("path        %s\n", where);
	printf("inode       %" PRIu64 "  parent %" PRIu64 "  private %" PRIu64 "\n",
	    ino.obj_id, ino.parent_id, ino.private_id);
	printf("mode        %06o  uid %u  gid %u\n", ino.mode, ino.owner, ino.group);
	printf("size        %" PRIu64 "  allocated %" PRIu64 "\n", ino.size,
	    ino.alloced_size);
	printf("flags       internal 0x%" PRIx64 "  bsd 0x%x%s\n", ino.internal_flags,
	    ino.bsd_flags, ino.is_sparse ? "  sparse" : "");
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	if (apfs_cfile_init(&vol, &ino, &cf) == 0 && cf.compressed) {
		printf("decmpfs     type %u  uncompressed %" PRIu64 "  %s\n", cf.type, cf.size,
		    cf.in_rsrc ? "in the resource fork" : "in the attribute");
	}
	apfs_cfile_fini(&cf);
	apfs_xattr_list(&vol, id, xattr_print_cb, NULL);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- cat ---- */

static int
cmd_cat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id, off = 0;
	uint8_t dt;
	uint8_t *buf;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error == 0) {
		error = apfs_inode_get(&vol, id, &ino);
	}
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	buf = malloc(65536);
	if (buf == NULL) {
		return 1;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	{
		uint64_t size = 0;

		/* (a compressed file's size is in its decmpfs header, not its inode) */
		if (apfs_inode_size(&vol, &ino, &size) == 0) {
			ino.size = size;
		}
	}
	while (off < ino.size) {
		size_t got = 0;

		error = apfs_read_file(&vol, &ino, &cf, off, buf, 65536, &got);
		if (error) {
			fprintf(stderr, "apfs-tool: read at %" PRIu64 ": %s\n", off,
			    strerror(error));
			free(buf);
			return 1;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, got, stdout) != got) {
			free(buf);
			return 1;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	free(buf);
	apfs_nx_close(&nx);
	return 0;
}

/*
 * ---- several threads on the library at once, as in the kernel ----
 *
 * The kernel holds one lock round apfs_lib for each operation and lets apfs_lib let go of it
 * in the middle (reading file contents, and a block of metadata not in the cache); a block
 * leaving the cache keeps its memory until no operation that began before can be reading it
 * (apfs_nx_reclaim). The same here, with a pthread mutex, so that it can be run under
 * ThreadSanitizer: op_begin/op_end are the kernel's apfs_lock/apfs_unlock.
 */
static pthread_mutex_t host_lock;      /* error checking: unlocking one not held aborts, as the kernel panics */
static struct apfs_nx *host_nx;

static void
host_lock_check(int error, const char *what)
{
	if (error != 0) {
		fprintf(stderr, "apfs-tool: %s the library's lock: %s\n", what, strerror(error));
		abort();
	}
}
#define HOST_OPS 64
static struct {
	pthread_t th;
	uint64_t  epoch;
	bool      used;
} host_ops[HOST_OPS];

static void
host_unlock(void *ctx)
{
	(void)ctx;
	host_lock_check(pthread_mutex_unlock(&host_lock), "letting go of");
}

static void
host_relock(void *ctx)
{
	(void)ctx;
	host_lock_check(pthread_mutex_lock(&host_lock), "taking");
}

static void
op_begin(void)
{
	host_lock_check(pthread_mutex_lock(&host_lock), "taking");
	for (int i = 0; i < HOST_OPS; i++) {
		if (!host_ops[i].used) {
			host_ops[i].used = true;
			host_ops[i].th = pthread_self();
			host_ops[i].epoch = host_nx->epoch;
			return;
		}
	}
	fprintf(stderr, "apfs-tool: more operations at once than %d\n", HOST_OPS);
	abort();
}

static void
op_end(void)
{
	uint64_t oldest = host_nx->epoch;

	for (int i = 0; i < HOST_OPS; i++) {
		if (host_ops[i].used && pthread_equal(host_ops[i].th, pthread_self())) {
			host_ops[i].used = false;
		} else if (host_ops[i].used && host_ops[i].epoch < oldest) {
			oldest = host_ops[i].epoch;
		}
	}
	apfs_nx_reclaim(host_nx, oldest);
	host_lock_check(pthread_mutex_unlock(&host_lock), "letting go of");
}

static void
host_locking(struct apfs_nx *nx)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&host_lock, &a);
	host_nx = nx;
	nx->unlock = host_unlock;
	nx->relock = host_relock;
	nx->lock_ctx = NULL;
}

/* ---- census: one line per entry, for diffing against apfs.py ---- */

struct census {
	struct apfs_vol *vol;
	char            *path;          /* the directory being walked, with a trailing / */
	size_t           path_len;
	size_t           path_cap;
	uint8_t         *buf;
	long             max;
	long             seen;
	size_t           chunk;         /* how much each read asks for */
	bool             meta_only;
	int              error;
};

static int census_dir(struct census *cs, uint64_t dir_id);
static int census_parallel(struct apfs_nx *nx, struct apfs_vol *vol, uint64_t start, const char *where,
    size_t chunk, bool meta_only, int n);

static uint64_t
file_hash(struct census *cs, const struct apfs_inode *ino, uint64_t size)
{
	uint64_t h = FNV_INIT, off = 0;
	struct apfs_cfile cf;

	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	for (;;) {
		size_t got = 0;
		int error = apfs_read_file(cs->vol, ino, &cf, off, cs->buf, cs->chunk, &got);

		if (error) {
			fprintf(stderr, "apfs-tool: %s: read at %" PRIu64 ": %s\n", cs->path,
			    off, strerror(error));
			cs->error = error;
			apfs_cfile_fini(&cf);
			return 0;
		}
		if (got == 0) {
			break;
		}
		h = fnv1a(cs->buf, got, h);
		off += got;
		if (off >= size) {
			break;
		}
	}
	apfs_cfile_fini(&cf);
	return h;
}

static int
census_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct census *cs = ctx;
	struct apfs_inode ino;
	size_t saved = cs->path_len;
	uint64_t h = 0, size = 0;
	int error;

	if (cs->max > 0 && cs->seen >= cs->max) {
		return EEXIST;
	}
	/* append "/name" to the path we are at */
	while (cs->path_len + name_len + 2 > cs->path_cap) {
		cs->path_cap *= 2;
		cs->path = realloc(cs->path, cs->path_cap);
		if (cs->path == NULL) {
			return ENOMEM;
		}
	}
	cs->path[cs->path_len++] = '/';
	memcpy(cs->path + cs->path_len, name, name_len);
	cs->path_len += name_len;
	cs->path[cs->path_len] = '\0';

	error = apfs_inode_get(cs->vol, file_id, &ino);
	if (error) {
		printf("%s\t?\tno-inode\n", cs->path);
		cs->error = error;
		goto done;
	}
	cs->seen++;
	if (apfs_inode_size(cs->vol, &ino, &size) != 0) {
		size = ino.size;
	}
	if (!cs->meta_only && ((ino.mode & APFS_S_IFMT) == APFS_S_IFREG ||
	    (ino.mode & APFS_S_IFMT) == APFS_S_IFLNK)) {
		h = file_hash(cs, &ino, size);
	}
	printf("%s\t%u\t%06o\t%u\t%u\t%" PRIu64 "\t%016" PRIx64 "\n", cs->path, d_type,
	    ino.mode, ino.owner, ino.group, size, h);

	if (d_type == APFS_DT_DIR) {
		error = census_dir(cs, file_id);
		if (error) {
			cs->path_len = saved;
			cs->path[cs->path_len] = '\0';
			return error;
		}
	}
done:
	cs->path_len = saved;
	cs->path[cs->path_len] = '\0';
	return 0;
}

static int
census_dir(struct census *cs, uint64_t dir_id)
{
	return apfs_readdir(cs->vol, dir_id, census_cb, cs);
}

static int
cmd_census(const char *path, const char *where, bool meta_only)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct census cs;
	uint64_t start = APFS_ROOT_DIR_INO_NUM;
	uint8_t dt = APFS_DT_DIR;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	if (where != NULL) {
		int e;

		if (want_lock) {
			op_begin();
		}
		e = apfs_path_lookup(&vol, where, &start, &dt);
		if (want_lock) {
			op_end();
		}
		if (e != 0) {
			fprintf(stderr, "apfs-tool: %s: not found\n", where);
			return 1;
		}
	}
	memset(&cs, 0, sizeof(cs));
	cs.vol = &vol;
	cs.path_cap = 4096;
	cs.path = malloc(cs.path_cap);
	/*
	 * APFS_TOOL_CHUNK: reads of another size than 64 KB (say 4096, or 12345 and 200000,
	 * which never line up with a compressed file's 64 KB pieces), so that the roads through
	 * apfs_read_file the census does not otherwise take are checked over a whole volume too.
	 */
	cs.chunk = 65536;
	if (getenv("APFS_TOOL_CHUNK") != NULL) {
		long c = strtol(getenv("APFS_TOOL_CHUNK"), NULL, 0);

		if (c < 1 || c > 16 * 1024 * 1024) {
			fprintf(stderr, "apfs-tool: APFS_TOOL_CHUNK %ld is not 1..16M\n", c);
			return 2;
		}
		cs.chunk = (size_t)c;
	}
	cs.buf = malloc(cs.chunk);
	cs.max = 0;
	cs.meta_only = meta_only;
	if (cs.path == NULL || cs.buf == NULL) {
		return 1;
	}
	cs.path[0] = '\0';
	if (where != NULL) {
		size_t n = strlen(where);

		while (n > 0 && where[n - 1] == '/') {
			n--;
		}
		memcpy(cs.path, where, n);
		cs.path_len = n;
		cs.path[n] = '\0';
	}
	if (getenv("APFS_TOOL_WALKERS") != NULL && strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0) > 1) {
		long n = strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0);

		return census_parallel(&nx, &vol, start, where, cs.chunk, meta_only, (int)(n > 16 ? 16 : n));
	}
	error = census_dir(&cs, start);
	if (error && error != EEXIST) {
		fprintf(stderr, "apfs-tool: census: %s\n", strerror(error));
		return 1;
	}
	fprintf(stderr, "apfs-tool: %ld entries, %" PRIu64 " blocks read, %" PRIu64
	    " cache hits; %" PRIu64 " neighbours brought in, %" PRIu64 " of them asked for\n",
	    cs.seen, nx.reads, nx.hits, nx.prefetched, nx.prefetch_used);
	fprintf(stderr, "apfs-tool: inflate %" PRIu64 " calls, %" PRIu64 " bytes in, %" PRIu64
	    " bytes out, %" PRIu64 " by zlib\n", nx.inflate_calls, nx.inflate_in, nx.inflate_out,
	    nx.inflate_fallbacks);
	fprintf(stderr, "apfs-tool: %" PRIu64 " requests of whole pieces, %" PRIu64 " pieces in "
	    "them, on %u threads at most; read ahead %" PRIu64 " times, %" PRIu64 " pieces, %"
	    PRIu64 " of them used\n", nx.batch_calls, nx.batch_pieces,
	    nx.parallel != NULL ? nx.parallel_max : 1, nx.ra_batches, nx.ra_pieces, nx.ra_hits);
	fprintf(stderr, "apfs-tool: highest block touched %" PRIu64 " (%.2f GB in), of which "
	    "file contents reach %" PRIu64 " (%.2f GB in)\n", nx.max_addr,
	    nx.max_addr * (double)nx.block_size / 1e9, nx.max_data_addr,
	    nx.max_data_addr * (double)nx.block_size / 1e9);
	free(cs.path);
	free(cs.buf);
	apfs_nx_close(&nx);
	return cs.error ? 1 : 0;
}

/*
 * APFS_TOOL_WALKERS=N with census: N threads walk at once, taking directories off one shared
 * stack, each line hashed and the hashes added up (order does not matter): the same sum the
 * kernel's apfs_check prints. Lines are not printed.
 */
struct pjob {
	uint64_t dir_id;
	char    *path;
};

static pthread_mutex_t pmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcond = PTHREAD_COND_INITIALIZER;
static struct pjob *pstack;
static size_t pcount, pcap;
static int pbusy;

struct pwalker {
	struct apfs_vol *vol;
	const char      *dir;           /* the directory being walked */
	uint8_t         *buf;
	size_t           chunk;
	bool             meta_only;
	uint64_t         sum, entries, bytes;
	int              error;
};

static void
ppush(uint64_t dir_id, const char *path)
{
	pthread_mutex_lock(&pmtx);
	if (pcount == pcap) {
		pcap = pcap ? 2 * pcap : 1024;
		pstack = realloc(pstack, pcap * sizeof(*pstack));
		if (pstack == NULL) {
			abort();
		}
	}
	pstack[pcount].dir_id = dir_id;
	pstack[pcount].path = strdup(path);
	pcount++;
	pthread_cond_signal(&pcond);
	pthread_mutex_unlock(&pmtx);
}

static uint64_t
pfile_hash(struct pwalker *w, const struct apfs_inode *ino, uint64_t size, const char *path)
{
	uint64_t h = FNV_INIT, off = 0;
	struct apfs_cfile cf;

	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	for (;;) {
		size_t got = 0;
		int error = apfs_read_file(w->vol, ino, &cf, off, w->buf, w->chunk, &got);

		if (error) {
			fprintf(stderr, "apfs-tool: %s: read at %" PRIu64 ": %s\n", path, off, strerror(error));
			w->error = error;
			break;
		}
		if (got == 0) {
			break;
		}
		h = fnv1a(w->buf, got, h);
		off += got;
		w->bytes += got;
		if (off >= size) {
			break;
		}
	}
	apfs_cfile_fini(&cf);
	return h;
}

static int
pcensus_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct pwalker *w = ctx;
	struct apfs_inode ino;
	uint64_t h = 0, size = 0;
	char path[4096], line[4096 + 128];
	int n;

	n = snprintf(path, sizeof(path), "%s/%.*s", w->dir, (int)name_len, name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		w->error = ENAMETOOLONG;
		return 0;
	}
	if (apfs_inode_get(w->vol, file_id, &ino) != 0) {
		fprintf(stderr, "apfs-tool: %s: no inode\n", path);
		w->error = ENOENT;
		return 0;
	}
	if (apfs_inode_size(w->vol, &ino, &size) != 0) {
		size = ino.size;
	}
	if (!w->meta_only && ((ino.mode & APFS_S_IFMT) == APFS_S_IFREG || (ino.mode & APFS_S_IFMT) == APFS_S_IFLNK)) {
		h = pfile_hash(w, &ino, size, path);
	}
	n = snprintf(line, sizeof(line), "%s\t%u\t%06o\t%u\t%u\t%" PRIu64 "\t%016" PRIx64 "\n", path, d_type,
	    ino.mode, ino.owner, ino.group, size, h);
	w->sum += fnv1a((const uint8_t *)line, (size_t)n, FNV_INIT);
	w->entries++;
	if (d_type == APFS_DT_DIR) {
		ppush(file_id, path);
	}
	return 0;
}

static void *
pwalker_thread(void *p)
{
	struct pwalker *w = p;

	for (;;) {
		struct pjob j;
		int error;

		pthread_mutex_lock(&pmtx);
		while (pcount == 0 && pbusy > 0) {
			pthread_cond_wait(&pcond, &pmtx);
		}
		if (pcount == 0) {
			pthread_cond_broadcast(&pcond);
			pthread_mutex_unlock(&pmtx);
			return NULL;
		}
		j = pstack[--pcount];
		pbusy++;
		pthread_mutex_unlock(&pmtx);

		w->dir = j.path;
		op_begin();
		error = apfs_readdir(w->vol, j.dir_id, pcensus_cb, w);
		op_end();
		if (error) {
			fprintf(stderr, "apfs-tool: %s: readdir: %s\n", j.path, strerror(error));
			w->error = error;
		}
		free(j.path);

		pthread_mutex_lock(&pmtx);
		pbusy--;
		pthread_cond_broadcast(&pcond);
		pthread_mutex_unlock(&pmtx);
	}
}

static int
census_parallel(struct apfs_nx *nx, struct apfs_vol *vol, uint64_t start, const char *where, size_t chunk,
    bool meta_only, int n)
{
	struct pwalker w[16];
	pthread_t th[16];
	uint64_t sum = 0, entries = 0, bytes = 0;
	int error = 0;

	ppush(start, where != NULL ? where : "");
	for (int i = 0; i < n; i++) {
		memset(&w[i], 0, sizeof(w[i]));
		w[i].vol = vol;
		w[i].chunk = chunk;
		w[i].meta_only = meta_only;
		w[i].buf = malloc(chunk);
		if (w[i].buf == NULL || pthread_create(&th[i], NULL, pwalker_thread, &w[i]) != 0) {
			abort();
		}
	}
	for (int i = 0; i < n; i++) {
		pthread_join(th[i], NULL);
		sum += w[i].sum;
		entries += w[i].entries;
		bytes += w[i].bytes;
		if (w[i].error) {
			error = w[i].error;
		}
		free(w[i].buf);
	}
	fprintf(stderr, "apfs-tool: %d walkers: %" PRIu64 " entries, %" PRIu64 " content bytes, line-hash sum %016" PRIx64
	    ", %s\n", n, entries, bytes, sum, error ? "ERRORS" : "no errors");
	fprintf(stderr, "apfs-tool: metadata read without the lock %" PRIu64 " times, %" PRIu64 " of them already in; "
	    "retired blocks %" PRIu64 " now, %" PRIu64 " at most\n", nx->unlocked_fills, nx->fill_races, nx->retired_now,
	    nx->retired_most);
	free(pstack);
	{
		int fd = (int)(intptr_t)nx->dev.ctx;

		apfs_nx_close(nx);
		close(fd);
	}
	return error ? 1 : 0;
}

/*
 * cfrace IMAGE N ROUNDS: the compressed files of the volume (up to 400, taken from a walk),
 * each read whole by N threads at once through one shared apfs_cfile -- as two readers of one
 * vnode share it in the kernel -- with the cache emptied before every round, so that setting
 * up the file and its read-ahead let go of the lock in the middle; every read must hash the
 * same as a read on one thread.
 */
struct cf_file {
	uint64_t           id;
	struct apfs_inode  ino;
	uint64_t           size;
	uint64_t           want;        /* the hash on one thread */
	struct apfs_cfile  cf;          /* shared by the threads, touched with the lock held */
};

static struct cf_file *cf_files;
static size_t cf_nfiles;
static struct apfs_vol *cf_vol;
static atomic_uint cf_bad, cf_reads;

static int
cf_collect_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t size;

	(void)ctx;
	(void)name;
	(void)name_len;
	if (cf_nfiles >= 400) {
		return EEXIST;
	}
	if (d_type == APFS_DT_DIR) {
		return apfs_readdir(cf_vol, file_id, cf_collect_cb, NULL) == EEXIST ? EEXIST : 0;
	}
	if (d_type != APFS_DT_REG || apfs_inode_get(cf_vol, file_id, &ino) != 0 ||
	    apfs_inode_size(cf_vol, &ino, &size) != 0 || size < 3 * 65536) {
		return 0;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	if (apfs_cfile_init(cf_vol, &ino, &cf) == 0 && cf.compressed && cf.in_rsrc) {
		cf_files[cf_nfiles].id = file_id;
		cf_files[cf_nfiles].ino = ino;
		cf_files[cf_nfiles].size = size;
		cf_nfiles++;
	}
	apfs_cfile_fini(&cf);
	return 0;
}

static uint64_t
cf_read_whole(struct cf_file *f, struct apfs_cfile *cf, uint8_t *buf, size_t chunk, bool locked)
{
	uint64_t h = FNV_INIT, off = 0;

	while (off < f->size) {
		size_t got = 0;
		int error;

		if (locked) {
			op_begin();
		}
		error = apfs_read_file(cf_vol, &f->ino, cf, off, buf, chunk, &got);
		if (locked) {
			op_end();
		}
		if (error || got == 0) {
			return 0;
		}
		h = fnv1a(buf, got, h);
		off += got;
	}
	return h;
}

static void *
cf_thread(void *p)
{
	uint8_t *buf = malloc(65536);

	(void)p;
	for (size_t i = 0; i < cf_nfiles; i++) {
		uint64_t h = cf_read_whole(&cf_files[i], &cf_files[i].cf, buf, 65536, true);

		atomic_fetch_add(&cf_reads, 1);
		if (h != cf_files[i].want) {
			atomic_fetch_add(&cf_bad, 1);
			fprintf(stderr, "apfs-tool: cfrace: inode %" PRIu64 " read DIFFERENTLY\n", cf_files[i].id);
		}
	}
	free(buf);
	return NULL;
}

static int
cmd_cfrace(const char *path, int n, int rounds)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	pthread_t th[16];
	uint8_t *buf = malloc(65536);

	want_lock = true;
	if (open_image(path, &nx, &vol) != 0 || buf == NULL || n < 2 || n > 16) {
		return 1;
	}
	cf_vol = &vol;
	cf_files = calloc(400, sizeof(*cf_files));
	op_begin();
	(void)apfs_readdir(&vol, APFS_ROOT_DIR_INO_NUM, cf_collect_cb, NULL);
	op_end();
	for (size_t i = 0; i < cf_nfiles; i++) {
		struct apfs_cfile cf;

		memset(&cf, 0, sizeof(cf));
		cf.held = APFS_NO_PIECE;
		cf_files[i].want = cf_read_whole(&cf_files[i], &cf, buf, 65536, true);
		op_begin();
		apfs_cfile_fini(&cf);
		op_end();
	}
	fprintf(stderr, "apfs-tool: cfrace: %zu compressed files of 192 KB or more, %d threads, %d rounds\n", cf_nfiles,
	    n, rounds);
	if (nx.parallel == NULL) {
		nx.parallel = host_parallel;    /* read-ahead needs helpers to spare */
		nx.parallel_max = 4;
	}
	for (int r = 0; r < rounds; r++) {
		pthread_mutex_lock(&host_lock);
		for (size_t i = 0; i < cf_nfiles; i++) {
			apfs_cfile_fini(&cf_files[i].cf);
		}
		apfs_nx_cache_flush(&nx);
		pthread_mutex_unlock(&host_lock);
		for (int t = 0; t < n; t++) {
			pthread_create(&th[t], NULL, cf_thread, NULL);
		}
		for (int t = 0; t < n; t++) {
			pthread_join(th[t], NULL);
		}
	}
	fprintf(stderr, "apfs-tool: cfrace: %u reads, %u different; metadata read without the lock %" PRIu64
	    " times, %" PRIu64 " of them already in; read ahead %" PRIu64 " times\n", atomic_load(&cf_reads),
	    atomic_load(&cf_bad), nx.unlocked_fills, nx.fill_races, nx.ra_batches);
	/* all given back, so that a leak checker sees only what the library lost */
	for (size_t i = 0; i < cf_nfiles; i++) {
		apfs_cfile_fini(&cf_files[i].cf);
	}
	free(cf_files);
	free(buf);
	{
		int fd = (int)(intptr_t)nx.dev.ctx;

		apfs_nx_close(&nx);
		close(fd);
	}
	return atomic_load(&cf_bad) != 0;
}

/*
 * namehash IMAGE OUT: every directory record's name hashed the way the lookup hashes it,
 * against the hash APFS stored with it; and into OUT, for tools/apfs/test_namehash.py to work
 * out independently, a line per record: directory, the name in hex, the stored hash, the hash
 * as on a case-sensitive volume and as on a case-insensitive one, the file's id.
 */
struct nh_ctx {
	FILE    *out;
	bool     fold;
	uint64_t total, ascii, other, same, differ, unhashable;
};

static int
namehash_cb(void *ctx, uint64_t dir_id, const char *name, size_t name_len, uint32_t stored, uint64_t file_id)
{
	struct nh_ctx *c = ctx;
	uint32_t cs = 0, ci = 0, mine;
	bool ok_cs = apfs_name_hash(name, name_len, false, &cs), ok_ci = apfs_name_hash(name, name_len, true, &ci);
	bool ascii = true;

	for (size_t i = 0; i < name_len; i++) {
		ascii &= (uint8_t)name[i] < 0x80;
	}
	c->total++;
	if (ascii) {
		c->ascii++;
	} else {
		c->other++;
	}
	mine = c->fold ? ci : cs;
	if (!(c->fold ? ok_ci : ok_cs)) {
		c->unhashable++;
	} else if (mine == stored) {
		c->same++;
	} else {
		c->differ++;
		fprintf(stderr, "apfs-tool: namehash: dir %" PRIu64 ": %.*s: stored %06x, worked out %06x\n", dir_id,
		    (int)name_len, name, stored, mine);
	}
	if (c->out != NULL) {
		fprintf(c->out, "%" PRIu64 "\t", dir_id);
		for (size_t i = 0; i < name_len; i++) {
			fprintf(c->out, "%02x", (uint8_t)name[i]);
		}
		fprintf(c->out, "\t%06x\t%06x\t%06x\t%" PRIu64 "\n", stored, ok_cs ? cs : 0xffffffu, ok_ci ? ci : 0xffffffu,
		    file_id);
	}
	return 0;
}

static int
cmd_namehash(const char *path, const char *out)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct nh_ctx c;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	memset(&c, 0, sizeof(c));
	c.fold = (vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
	c.out = out != NULL ? fopen(out, "w") : NULL;
	error = apfs_dirrec_walk(&vol, namehash_cb, &c);
	if (c.out != NULL) {
		fclose(c.out);
	}
	fprintf(stderr, "apfs-tool: namehash: %s volume: %" PRIu64 " directory records (%" PRIu64 " plain ASCII, %" PRIu64
	    " not): hash as stored %" PRIu64 ", not %" PRIu64 ", cannot be hashed %" PRIu64 "%s\n",
	    c.fold ? "case-insensitive" : "case-sensitive", c.total, c.ascii, c.other, c.same, c.differ, c.unhashable,
	    error ? " (WALK FAILED)" : "");
	return (error || c.differ || c.unhashable) ? 1 : 0;
}

/*
 * lookupnames IMAGE LIST: a line of LIST is a directory, a name in hex and what looking it up
 * must give (an id, or "none"); each looked up the way the kernel's vnop_lookup does it.
 */
static int
cmd_lookupnames(const char *path, const char *list)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	FILE *f = fopen(list, "r");
	char line[4096];
	uint64_t total = 0, right = 0, hashed0, scanned0;

	if (f == NULL || open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	hashed0 = nx.lookups_hashed;
	scanned0 = nx.lookups_scanned;
	while (fgets(line, sizeof(line), f) != NULL) {
		char hex[2048], want[32], name[1024];
		uint64_t dir, id = 0;
		size_t n = 0;
		uint8_t dt;
		int error;

		if (sscanf(line, "%" SCNu64 " %2047s %31s", &dir, hex, want) != 3) {
			continue;
		}
		for (size_t i = 0; hex[2 * i] != 0 && hex[2 * i + 1] != 0 && n < sizeof(name); i++) {
			unsigned v;

			sscanf(hex + 2 * i, "%2x", &v);
			name[n++] = (char)v;
		}
		total++;
		error = apfs_lookup(&vol, dir, name, n, &id, &dt);
		if ((strcmp(want, "none") == 0 && error == ENOENT) ||
		    (strcmp(want, "none") != 0 && error == 0 && id == strtoull(want, NULL, 0))) {
			right++;
		} else if (total - right <= 20) {
			fprintf(stderr, "apfs-tool: lookupnames: dir %" PRIu64 " %s: want %s, got %s %" PRIu64 "\n", dir, hex, want,
			    error ? strerror(error) : "id", id);
		}
	}
	fclose(f);
	fprintf(stderr, "apfs-tool: lookupnames: %" PRIu64 " names, %" PRIu64 " as expected, %" PRIu64 " not; %" PRIu64
	    " found by the hash, %" PRIu64 " by reading the directory\n", total, right, total - right,
	    nx.lookups_hashed - hashed0, nx.lookups_scanned - scanned0);
	return right == total ? 0 : 1;
}

/*
 * Every name in the volume looked up by name, the way the kernel's vnop_lookup does it,
 * against what reading its directory gave; and each name with "#" after it, which must
 * not be found.  Also how long that took against finding each name by reading its
 * directory from the start, which is what apfs_lookup did before it went by the hash.
 */
struct lk_entry {
	char     name[256];
	size_t   len;
	uint64_t id;
	uint8_t  type;
};

struct lk_dir {
	struct lk_entry *e;
	size_t n, cap;
};

static int
lk_collect(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct lk_dir *d = ctx;

	if (d->n == d->cap) {
		size_t cap = d->cap ? d->cap * 2 : 64;
		struct lk_entry *e = realloc(d->e, cap * sizeof(*e));

		if (e == NULL) {
			return ENOMEM;
		}
		d->e = e;
		d->cap = cap;
	}
	if (name_len >= sizeof(d->e[0].name)) {
		return EIO;
	}
	memcpy(d->e[d->n].name, name, name_len);
	d->e[d->n].name[name_len] = '\0';
	d->e[d->n].len = name_len;
	d->e[d->n].id = file_id;
	d->e[d->n].type = d_type;
	d->n++;
	return 0;
}

struct lk_scan {
	const char *name;
	size_t      len;
	bool        found;
};

static int
lk_scan_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct lk_scan *sc = ctx;

	(void)file_id;
	(void)d_type;
	if (name_len == sc->len && memcmp(name, sc->name, name_len) == 0) {
		sc->found = true;
		return EEXIST;
	}
	return 0;
}

static double
lk_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
cmd_lookups(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	uint64_t *stack = NULL;
	size_t depth = 0, cap = 0;
	uint64_t dirs = 0, names = 0, good = 0, bad = 0, absent_good = 0, absent_bad = 0;
	double t_hash = 0, t_scan = 0;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	stack = malloc(sizeof(*stack) * 64);
	cap = 64;
	stack[depth++] = APFS_ROOT_DIR_INO_NUM;
	while (depth > 0) {
		uint64_t dir = stack[--depth];
		struct lk_dir d = { 0 };
		size_t i;
		int error = apfs_readdir(&vol, dir, lk_collect, &d);

		if (error) {
			fprintf(stderr, "apfs-tool: readdir %" PRIu64 ": %s\n", dir, strerror(error));
			bad++;
			continue;
		}
		dirs++;
		for (i = 0; i < d.n; i++) {
			struct lk_entry *e = &d.e[i];
			struct lk_scan sc = { e->name, e->len, false };
			char absent[260];
			uint64_t id = 0;
			uint8_t type = 0;
			double t0;

			names++;
			t0 = lk_now();
			error = apfs_lookup(&vol, dir, e->name, e->len, &id, &type);
			t_hash += lk_now() - t0;
			if (error == 0 && id == e->id && type == e->type) {
				good++;
			} else {
				bad++;
				if (bad <= 10) {
					fprintf(stderr, "apfs-tool: lookup %s in %" PRIu64 ": %d, %" PRIu64
					    " (readdir: %" PRIu64 ")\n", e->name, dir, error, id, e->id);
				}
			}
			t0 = lk_now();
			error = apfs_readdir(&vol, dir, lk_scan_cb, &sc);
			t_scan += lk_now() - t0;
			if (!sc.found) {
				bad++;
			}
			snprintf(absent, sizeof(absent), "%s#", e->name);
			error = apfs_lookup(&vol, dir, absent, e->len + 1, NULL, NULL);
			if (error == ENOENT) {
				absent_good++;
			} else {
				absent_bad++;
			}
			if (e->type == APFS_DT_DIR) {
				if (depth == cap) {
					cap *= 2;
					stack = realloc(stack, sizeof(*stack) * cap);
				}
				stack[depth++] = e->id;
			}
		}
		free(d.e);
	}
	printf("directories %" PRIu64 ", names %" PRIu64 ": found as readdir has them %" PRIu64
	    ", not %" PRIu64 "; with \"#\" after them not found %" PRIu64 ", found %" PRIu64 "\n",
	    dirs, names, good, bad, absent_good, absent_bad);
	printf("time for all of them: apfs_lookup %.0f ms, reading the directory from its start "
	    "%.0f ms\n", t_hash * 1000, t_scan * 1000);
	free(stack);
	apfs_nx_close(&nx);
	return (bad == 0 && absent_bad == 0) ? 0 : 1;
}

/*
 * Replace bytes inside a file of the image: the kernel's own apfs_write.c, run here where a
 * ruined image costs nothing.  The bytes come from a file or from standard input.
 * --fail-after N stops the device after N blocks, which is this program's power failure.
 */
static int
cmd_write(const char *path, const char *in_path, const char *at_str, const char *from)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_write_report rep;
	uint64_t at = strtoull(at_str, NULL, 0);
	uint8_t *data = NULL;
	size_t len = 0, cap = 0;
	uint64_t id = 0;
	uint8_t type = 0;
	FILE *f;
	int error;

	f = strcmp(from, "-") == 0 ? stdin : fopen(from, "rb");
	if (f == NULL) {
		perror(from);
		return 1;
	}
	for (;;) {
		size_t n;

		if (len == cap) {
			cap = cap ? cap * 2 : 65536;
			data = realloc(data, cap);
			if (data == NULL) {
				fprintf(stderr, "apfs-tool: out of memory\n");
				return 1;
			}
		}
		n = fread(data + len, 1, cap - len, f);
		len += n;
		if (n == 0) {
			break;
		}
	}
	if (f != stdin) {
		fclose(f);
	}

	want_write = true;
	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	if (apfs_path_lookup(&vol, in_path, &id, &type) != 0) {
		fprintf(stderr, "apfs-tool: %s: no such path in the volume\n", in_path);
		return 1;
	}
	error = apfs_write_in_place(&vol, id, at, data, len, &rep);
	printf("%s: %zu bytes at %" PRIu64 " of %s (inode %" PRIu64 "): %s\n", path, len, at,
	    in_path, id, error == 0 ? "written" : (rep.why ? rep.why : strerror(error)));
	printf("  blocks %" PRIu64 " from %" PRIu64 ", of them patched whole %" PRIu64
	    ", writes %" PRIu64 "\n", rep.blocks, rep.first_block, rep.partial, rep.calls);
	apfs_nx_close(&nx);
	free(data);
	return error == 0 ? 0 : 1;
}

/* Would this volume take a write at all, and what does it say about itself? */
static int
cmd_writable(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	const char *why = NULL;
	int error;

	want_write = true;
	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_write_volume_ok(&vol, &why);
	printf("%s: volume %s: %s\n", path, vol.name, error == 0 ? "writable in place" : why);
	printf("  incompatible 0x%" PRIx64 "  snapshots %" PRIu64 "  integrity metadata %" PRIu64
	    "  extent-reference tree %s\n", vol.incompat, vol.num_snapshots, vol.integrity_meta_oid,
	    vol.extentref_oid ? "yes" : "no");
	printf("  checkpoint rings: descriptor %" PRIu64 "+%u, data %" PRIu64 "+%u; container omap %"
	    PRIu64 "\n", nx.xp_desc_base, nx.xp_desc_blocks, nx.xp_data_base, nx.xp_data_blocks,
	    nx.omap_addr);
	apfs_nx_close(&nx);
	return error == 0 ? 0 : 1;
}


/* ---- the store of fixed-length slot files (bsd/miscfs/apfs/apfs_store.c) ---- */

#define STORE_SCRATCH (1u << 20)

static int
store_open(const char *path, struct apfs_nx *nx, struct apfs_vol *vol, struct apfs_store *st,
    bool for_writing)
{
	const char *why = NULL;

	want_write = for_writing;
	if (open_image(path, nx, vol) != 0) {
		return 1;
	}
	if (apfs_store_open(vol, st, &why) != 0) {
		fprintf(stderr, "apfs-tool: %s is not a store: %s\n", path,
		    why != NULL ? why : "no reason given");
		return 1;
	}
	return 0;
}

static int
cmd_store_info(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_head h;
	uint8_t *scratch;
	int slot, best;

	if (store_open(path, &nx, &vol, &st, false) != 0) {
		return 1;
	}
	{
		const char *why = NULL;

		printf("%s: store on volume %s: %d slots of %" PRIu64 " bytes, %" PRIu64
		    " of payload; a save here: %s\n", path, vol.name, APFS_STORE_SLOTS,
		    st.slot_bytes, st.payload_max,
		    apfs_store_writable(&st, &why) == 0 ? "would go through" : why);
	}
	for (slot = 0; slot < APFS_STORE_SLOTS; slot++) {
		if (apfs_store_head(&st, slot, &h) != 0) {
			printf("  slot %d (inode %" PRIu64 "): nothing good\n", slot,
			    st.slot_id[slot]);
			continue;
		}
		printf("  slot %d (inode %" PRIu64 "): generation %" PRIu64 ", %" PRIu64
		    " bytes, sum %016" PRIx64 ", note %" PRIu64 ", at %" PRIu64 "\n", slot,
		    st.slot_id[slot], h.generation, h.bytes, h.sum, h.note, h.seconds);
	}
	scratch = malloc(STORE_SCRATCH);
	best = scratch == NULL ? -1 : apfs_store_newest(&st, scratch, STORE_SCRATCH, &h);
	free(scratch);
	if (best < 0) {
		printf("  the newest good slot: none\n");
	} else {
		printf("  the newest good slot: %d, generation %" PRIu64 ", %" PRIu64 " bytes\n",
		    best, h.generation, h.bytes);
	}
	apfs_nx_close(&nx);
	return 0;
}

static int
cmd_store_save(const char *path, const char *from, uint64_t note)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_write w;
	uint8_t *data = NULL, *scratch;
	size_t len = 0, cap = 0;
	FILE *f = strcmp(from, "-") == 0 ? stdin : fopen(from, "rb");
	int error;

	if (f == NULL) {
		perror(from);
		return 1;
	}
	for (;;) {
		size_t n;

		if (len == cap) {
			cap = cap ? cap * 2 : (1u << 16);
			data = realloc(data, cap);
			if (data == NULL) {
				fprintf(stderr, "apfs-tool: out of memory\n");
				return 1;
			}
		}
		n = fread(data + len, 1, cap - len, f);
		len += n;
		if (n == 0) {
			break;
		}
	}
	if (f != stdin) {
		fclose(f);
	}
	if (store_open(path, &nx, &vol, &st, true) != 0) {
		return 1;
	}
	scratch = malloc(STORE_SCRATCH);
	if (scratch == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	error = apfs_store_save(&st, data, len, 0, note, scratch, STORE_SCRATCH, &w);
	printf("%s: %zu bytes into slot %d as generation %" PRIu64 ": %s; %" PRIu64 " blocks\n",
	    path, len, w.slot, w.generation, error == 0 ? "saved" : strerror(error), w.blocks);
	free(scratch);
	free(data);
	apfs_nx_close(&nx);
	return error == 0 ? 0 : 1;
}

static int
cmd_store_load(const char *path, const char *to)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_head h;
	uint8_t *scratch, *buf;
	FILE *f;
	int best, error;

	if (store_open(path, &nx, &vol, &st, false) != 0) {
		return 1;
	}
	scratch = malloc(STORE_SCRATCH);
	if (scratch == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	best = apfs_store_newest(&st, scratch, STORE_SCRATCH, &h);
	free(scratch);
	if (best < 0) {
		fprintf(stderr, "apfs-tool: %s: no slot holds anything good\n", path);
		return 1;
	}
	buf = malloc(h.bytes ? (size_t)h.bytes : 1);
	if (buf == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	error = h.bytes == 0 ? 0 : apfs_store_read(&st, best, 0, buf, (size_t)h.bytes);
	if (error) {
		fprintf(stderr, "apfs-tool: slot %d will not read: %s\n", best, strerror(error));
		return 1;
	}
	f = strcmp(to, "-") == 0 ? stdout : fopen(to, "wb");
	if (f == NULL) {
		perror(to);
		return 1;
	}
	if (h.bytes != 0 && fwrite(buf, 1, (size_t)h.bytes, f) != h.bytes) {
		perror(to);
		return 1;
	}
	if (f != stdout) {
		fclose(f);
	}
	fprintf(stderr, "%s: slot %d, generation %" PRIu64 ", %" PRIu64 " bytes, note %" PRIu64
	    "\n", path, best, h.generation, h.bytes, h.note);
	free(buf);
	apfs_nx_close(&nx);
	return 0;
}

int
main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "--fail-after") == 0 ||
		    strcmp(argv[i], "--corrupt-after") == 0) && i + 1 < argc) {
			write_fail_after = strtoull(argv[i + 1], NULL, 0);
			if (argv[i][2] == 'f') {
				write_fails = true;
			} else {
				write_corrupts = true;
			}
			memmove(&argv[i], &argv[i + 2], (size_t)(argc - i - 2) * sizeof(*argv));
			argc -= 2;
			i--;
		}
	}
	if (argc < 3) {
		fprintf(stderr,
		    "usage: apfs-tool info   IMAGE\n"
		    "       apfs-tool ls     IMAGE [PATH]\n"
		    "       apfs-tool stat   IMAGE PATH\n"
		    "       apfs-tool cat    IMAGE PATH\n"
		    "       apfs-tool census IMAGE [PATH] [--meta]\n"
		    "       apfs-tool write  IMAGE PATH OFFSET FILE|- [--fail-after N | --corrupt-after N]\n"
		    "       apfs-tool writable IMAGE\n"
		    "       apfs-tool store  info IMAGE | save IMAGE FILE|- [NOTE] | load IMAGE FILE|-\n"
		    "       apfs-tool lookups IMAGE\n"
		    "       apfs-tool cfrace IMAGE [THREADS [ROUNDS]]\n"
		    "       apfs-tool namehash IMAGE [OUT]\n"
		    "       apfs-tool lookupnames IMAGE LIST\n");
		return 2;
	}
	if (strcmp(argv[1], "info") == 0) {
		return cmd_info(argv[2]);
	}
	if (strcmp(argv[1], "ls") == 0) {
		return cmd_ls(argv[2], argc > 3 ? argv[3] : "/");
	}
	if (strcmp(argv[1], "stat") == 0 && argc > 3) {
		return cmd_stat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "cat") == 0 && argc > 3) {
		return cmd_cat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "census") == 0) {
		return cmd_census(argv[2], (argc > 3 && argv[3][0] == '/') ? argv[3] : NULL,
		    (argc > 3 && strcmp(argv[argc - 1], "--meta") == 0));
	}
	if (strcmp(argv[1], "write") == 0 && argc > 5) {
		return cmd_write(argv[2], argv[3], argv[4], argv[5]);
	}
	if (strcmp(argv[1], "writable") == 0) {
		return cmd_writable(argv[2]);
	}
	if (strcmp(argv[1], "store") == 0 && argc > 3) {
		if (strcmp(argv[2], "info") == 0) {
			return cmd_store_info(argv[3]);
		}
		if (strcmp(argv[2], "save") == 0 && argc > 4) {
			return cmd_store_save(argv[3], argv[4],
			    argc > 5 ? strtoull(argv[5], NULL, 0) : 0);
		}
		if (strcmp(argv[2], "load") == 0 && argc > 4) {
			return cmd_store_load(argv[3], argv[4]);
		}
		fprintf(stderr, "apfs-tool: store: info, save or load\n");
		return 2;
	}
	if (strcmp(argv[1], "lookups") == 0) {
		return cmd_lookups(argv[2]);
	}
	if (strcmp(argv[1], "namehash") == 0) {
		return cmd_namehash(argv[2], argc > 3 ? argv[3] : NULL);
	}
	if (strcmp(argv[1], "lookupnames") == 0 && argc > 3) {
		return cmd_lookupnames(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "cfrace") == 0) {
		return cmd_cfrace(argv[2], argc > 3 ? atoi(argv[3]) : 4, argc > 4 ? atoi(argv[4]) : 3);
	}
	fprintf(stderr, "apfs-tool: unknown command %s\n", argv[1]);
	return 2;
}
