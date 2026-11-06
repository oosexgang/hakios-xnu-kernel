/*
 * Read files off the APFS mount and hash them, two ways, so that what the driver hands
 * back can be compared with what tools/apfs/apfs.py reads from the same image on the
 * build machine.
 *
 *   mmap   map the file and read it through the mapping.  This is the only way to make
 *          the driver's vnop_pagein run: the walk inside the kernel (boot-arg apfs_check)
 *          goes through vnop_read, and nothing on this volume is executed, so without
 *          this the paging path is never touched at all.
 *   read   read(2) the file in pieces of a given size.  With a small size this is the
 *          case the compressed-file path is worst at -- every read decompresses the whole
 *          64 KB piece it falls in.
 *
 * The hash is FNV-1a over the whole file, which is what verify.py and the in-kernel walk
 * use, so a line here can be compared with a line from either.
 *
 * usage: apfs_mmap_test [-q] mmap [PATH]
 *        apfs_mmap_test [-q] read [PATH] [CHUNK]
 *        apfs_mmap_test drop [PATH]
 * -q: no line for each file, only the files' count, bytes, line-hash sum and time.
 * drop: each file's pages are put out of the page cache (a shared mapping of it, msync with
 * MS_INVALIDATE), so that the next pass over PATH reads it from the volume again: a cold pass
 * without a reboot.  Nothing is hashed, and the driver's counters are left alone.
 * PATH defaults to /private/var/apfs-check/usr, CHUNK to 4096.  PATH may be anywhere on a
 * mounted volume: the lines name files from that volume's mount point (statfs), which is
 * what makes them the host's lines.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

#define MOUNT "/private/var/apfs-check"
static size_t mount_len = sizeof(MOUNT) - 1;   /* the mount point's length in a path */

static const char *tag;         /* what to print each line under */
static int chunk_size = 4096;
static int use_mmap;
static int quiet;               /* -q: no line a file, only what they add up to */
static int drop_mode;           /* drop: out of the page cache, nothing read */

static unsigned long long total_sum;
static unsigned long long total_bytes;
static long total_files, total_errors;

static unsigned long long
fnv1a(const unsigned char *p, size_t n, unsigned long long h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

/*
 * The driver's own breakdown of where the time went (kern.apfs_stats): waiting for the
 * device, decompressing, and the rest.  Zeroed before a road is walked and printed after,
 * so the numbers belong to that road alone.
 */
static void
stats_reset(void)
{
	char zero = 0;

	if (sysctlbyname("kern.apfs_stats", NULL, NULL, &zero, 1) != 0) {
		printf("%s cannot zero kern.apfs_stats: %s\n", tag, strerror(errno));
	}
}

static void
stats_print(void)
{
	static char buf[16384];
	size_t n = sizeof(buf);
	char *line, *next;

	if (sysctlbyname("kern.apfs_stats", buf, &n, NULL, 0) != 0) {
		printf("%s cannot read kern.apfs_stats: %s\n", tag, strerror(errno));
		return;
	}
	buf[sizeof(buf) - 1] = '\0';
	for (line = buf; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		}
		/* -q: the totals only (the per-caller and per-process tables are for vol_ls sysctl) */
		if (quiet && strncmp(line, "pagein ", 7) != 0 && strncmp(line, "read ", 5) != 0 &&
		    strncmp(line, "device ", 7) != 0 && strncmp(line, "data ", 5) != 0 &&
		    strncmp(line, "inflate ", 8) != 0 && strncmp(line, "blocks ", 7) != 0 &&
		    !(strncmp(line, "lock ", 5) == 0 && strstr(line, " takes ") != NULL &&
		    strstr(line, "metadata") != NULL)) {
			continue;
		}
		if (*line != '\0') {
			printf("%s kernel: %s\n", tag, line);
		}
	}
}

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* The bytes of one file, through a mapping. */
static int
hash_mapped(const char *path, off_t size, unsigned long long *out)
{
	int fd = open(path, O_RDONLY);
	void *map;

	if (fd < 0) {
		return -1;
	}
	if (size == 0) {
		close(fd);
		*out = FNV_INIT;        /* nothing to map, and nothing to hash */
		return 0;
	}
	map = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return -1;
	}
	/* touching every page is what makes the kernel page the file in */
	*out = fnv1a((const unsigned char *)map, (size_t)size, FNV_INIT);
	munmap(map, (size_t)size);
	return 0;
}

/* The bytes of one file, read in pieces. */
static int
hash_read(const char *path, off_t size, unsigned long long *out)
{
	int fd = open(path, O_RDONLY);
	unsigned long long h = FNV_INIT;
	unsigned char *buf;
	ssize_t n;

	(void)size;
	if (fd < 0) {
		return -1;
	}
	buf = malloc((size_t)chunk_size);
	if (buf == NULL) {
		close(fd);
		return -1;
	}
	while ((n = read(fd, buf, (size_t)chunk_size)) > 0) {
		h = fnv1a(buf, (size_t)n, h);
	}
	free(buf);
	close(fd);
	if (n < 0) {
		return -1;
	}
	*out = h;
	return 0;
}

/* One file's pages out of the page cache: they are the same ones every mapping of it shares. */
static int
drop_file(const char *path, off_t size)
{
	int fd = open(path, O_RDONLY);
	void *map;
	int rc;

	if (fd < 0) {
		return -1;
	}
	if (size == 0) {
		close(fd);
		return 0;
	}
	map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return -1;
	}
	rc = msync(map, (size_t)size, MS_INVALIDATE);
	munmap(map, (size_t)size);
	return rc;
}

static void
do_file(const char *path, off_t size)
{
	unsigned long long h = 0;
	char line[2048];
	int rc;

	if (drop_mode) {
		if (drop_file(path, size) != 0) {
			printf("%s %s: cannot drop it: %s\n", tag, path + mount_len, strerror(errno));
			total_errors++;
			return;
		}
		total_files++;
		total_bytes += (unsigned long long)size;
		return;
	}
	rc = use_mmap ? hash_mapped(path, size, &h) : hash_read(path, size, &h);

	if (rc != 0) {
		printf("%s %s: cannot read it: %s\n", tag, path + mount_len,
		    strerror(errno));
		total_errors++;
		return;
	}
	snprintf(line, sizeof(line), "%s\t%lld\t%016llx\n", path + mount_len,
	    (long long)size, h);
	if (!quiet) {
		printf("%s %s", tag, line);
	}
	total_sum += fnv1a((const unsigned char *)line, strlen(line), FNV_INIT);
	total_files++;
	total_bytes += (unsigned long long)size;
}

static void
walk(char *path, size_t len)
{
	DIR *d = opendir(path);
	struct dirent *de;

	if (d == NULL) {
		printf("%s %s: cannot open the directory: %s\n", tag,
		    path + mount_len, strerror(errno));
		total_errors++;
		return;
	}
	while ((de = readdir(d)) != NULL) {
		struct stat st;
		size_t n = strlen(de->d_name);

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (len + n + 2 > PATH_MAX) {
			total_errors++;
			continue;
		}
		path[len] = '/';
		memcpy(path + len + 1, de->d_name, n + 1);
		/* lstat, not stat: a symbolic link is not followed and not read */
		if (lstat(path, &st) != 0) {
			printf("%s %s: lstat failed: %s\n", tag, path + mount_len,
			    strerror(errno));
			total_errors++;
		} else if (S_ISDIR(st.st_mode)) {
			walk(path, len + 1 + n);
		} else if (S_ISREG(st.st_mode)) {
			do_file(path, st.st_size);
		}
		path[len] = '\0';
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	static char path[PATH_MAX];
	const char *where = MOUNT "/usr";
	const char *mode;
	double t0, t1;
	int tries;

	if (argc > 1 && strcmp(argv[1], "-q") == 0) {
		quiet = 1;
		argv++;
		argc--;
	}
	mode = argc > 1 ? argv[1] : "mmap";

	drop_mode = (strcmp(mode, "drop") == 0);
	use_mmap = (strcmp(mode, "read") != 0);
	tag = drop_mode ? "apfs-drop:" : use_mmap ? "apfs-mmap:" : "apfs-read:";
	if (argc > 2) {
		where = argv[2];
	}
	if (argc > 3) {
		chunk_size = atoi(argv[3]);
		if (chunk_size <= 0) {
			chunk_size = 4096;
		}
	}
	/* the kernel mounts the volume as soon as /private/var is there; wait for it */
	for (tries = 0; tries < 120; tries++) {
		struct stat st;

		if (stat(where, &st) == 0) {
			break;
		}
		sleep(1);
	}
	{
		struct statfs sf;

		if (statfs(where, &sf) == 0 && strncmp(where, sf.f_mntonname,
		    strlen(sf.f_mntonname)) == 0) {
			mount_len = strcmp(sf.f_mntonname, "/") == 0 ? 0 : strlen(sf.f_mntonname);
			printf("%s %s is %s on %s\n", tag, sf.f_mntonname, sf.f_mntfromname,
			    sf.f_fstypename);
		}
	}

	if (drop_mode) {
		snprintf(path, sizeof(path), "%s", where);
		t0 = now_ms();
		walk(path, strlen(path));
		t1 = now_ms();
		printf("%s %s: files %ld  bytes %llu  errors %ld, out of the page cache in %.0f ms\n", tag, where,
		    total_files, total_bytes, total_errors, t1 - t0);
		return total_errors ? 1 : 0;
	}
	printf("%s %s, %s%s\n", tag, where, use_mmap ? "through a mapping" : "with read(2)",
	    use_mmap ? "" : " in pieces");
	if (!use_mmap) {
		printf("%s pieces of %d bytes\n", tag, chunk_size);
	}
	snprintf(path, sizeof(path), "%s", where);
	stats_reset();
	t0 = now_ms();
	walk(path, strlen(path));
	t1 = now_ms();

	printf("%s files %ld  bytes %llu  errors %ld\n", tag, total_files, total_bytes,
	    total_errors);
	printf("%s line-hash sum %016llx\n", tag, total_sum);
	printf("%s done in %.0f ms (%.1f MB/s)\n", tag, t1 - t0,
	    (t1 > t0) ? (double)total_bytes / 1048576.0 / ((t1 - t0) / 1000.0) : 0.0);
	stats_print();
	return total_errors ? 1 : 0;
}
