/*
 * Host tests of the data volume's store (init/var_store.c), as it is: what `var_store erase` leaves in the partition,
 * whether the keychain is packed, and what `load` makes of an archive: that what is packed can be read back, and that
 * nothing is written through a symbolic link init made in /private/var.
 *
 * The program is compiled against libmini's header, as for the phone, and the host's C library answers the calls
 * it declares: what Linux has not got (sysctlbyname, chflags, the extended stat) is stood in for here, sysctlbyname
 * with a boot line of the test's choosing. The partition is a sparse file, with the slots at their real offsets.
 * The calls on files are the test's (t_open and the rest, as in init_test.c): /private/var is taken to a directory
 * of the test's, Darwin's open flags are turned into Linux's, and the extended attributes are a list of the test's.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"  /* (libmini's strerror gives a const string) */
#endif
#define main var_store_main
#define errno var_store_errno           /* (the host's errno is not a plain int) */
#define open t_open
#define lstat t_lstat
#define fstat t_fstat
#define mkdir t_mkdir
#define chown t_chown
#define lchown t_lchown
#define chmod t_chmod
#define unlink t_unlink
#define symlink t_symlink
#define link t_link
#define utimes t_utimes
#define setxattr t_setxattr
#define listxattr t_listxattr
#define getxattr t_getxattr
#include "../init/var_store.c"
#undef main
#undef errno
#undef open
#undef lstat
#undef fstat
#undef mkdir
#undef chown
#undef lchown
#undef chmod
#undef unlink
#undef symlink
#undef link
#undef utimes
#undef setxattr
#undef listxattr
#undef getxattr

int var_store_errno;
char *getenv(const char *name);

/* the host's own, which mini.h declares under the names above */
extern int *__errno_location(void);
int     snprintf(char *buf, size_t size, const char *format, ...);
int     system(const char *command);
int     open(const char *path, int flags, ...);
int     unlink(const char *path);
int     mkdir(const char *path, unsigned mode);
int     chown(const char *path, unsigned uid, unsigned gid);
int     lchown(const char *path, unsigned uid, unsigned gid);
int     chmod(const char *path, unsigned mode);
int     symlink(const char *target, const char *path);
int     link(const char *from, const char *to);
int     utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);

#define HOST_O_CREAT    0x40            /* Linux's, not Darwin's: the file is opened by the host's C library */
#define HOST_O_TRUNC    0x200
#if defined(__aarch64__) || defined(__arm__)
#define HOST_O_DIRECTORY 0x4000
#define HOST_O_NOFOLLOW 0x8000
#else
#define HOST_O_DIRECTORY 0x10000
#define HOST_O_NOFOLLOW 0x20000
#endif

/* Linux's struct statx, whose layout is the same on every architecture (struct stat's is not) */
struct host_statx {
	uint32_t        mask, blksize;
	uint64_t        attributes;
	uint32_t        nlink, uid, gid;
	uint16_t        mode, spare0;
	uint64_t        ino, size, blocks, attributes_mask;
	struct { int64_t sec; uint32_t nsec; int32_t reserved; } atime, btime, ctime, mtime;
	uint32_t        rdev_major, rdev_minor, dev_major, dev_minor;
	uint64_t        spare[14];
};
int     statx(int dirfd, const char *path, int flags, unsigned mask, struct host_statx *buf);
#define HOST_AT_FDCWD   (-100)
#define HOST_AT_SYMLINK_NOFOLLOW 0x100
#define HOST_AT_EMPTY_PATH 0x1000
#define HOST_STATX_BASIC 0x7ff

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *boot_line = "";

/* ---- the calls on files, the test's way ---- */

static char test_root[512];             /* /private/var is test_root/var */

/* PATH as the host is to see it */
static const char *
host_path(const char *p)
{
	static char buf[4][4096];
	static int next;
	char *o = buf[next++ % 4];

	if (strncmp(p, ROOT, sizeof(ROOT) - 1) == 0 && (p[sizeof(ROOT) - 1] == 0 || p[sizeof(ROOT) - 1] == '/')) {
		snprintf(o, sizeof(buf[0]), "%s/var%s", test_root, p + sizeof(ROOT) - 1);
		return o;
	}
	return p;
}

/* the result of a host call, its errno where var_store looks for it */
static int
answer(int result)
{
	if (result < 0) {
		var_store_errno = *__errno_location();
	}
	return result;
}

int
t_open(const char *p, int flags, int mode)
{
	int host = flags & 3;

	host |= flags & O_CREAT ? HOST_O_CREAT : 0;
	host |= flags & O_TRUNC ? HOST_O_TRUNC : 0;
	host |= flags & O_DIRECTORY ? HOST_O_DIRECTORY : 0;
	host |= flags & O_NOFOLLOW ? HOST_O_NOFOLLOW : 0;
	return answer(open(host_path(p), host, mode));
}

static void
from_statx(const struct host_statx *x, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = x->mode;
	st->st_nlink = (uint16_t)x->nlink;
	st->st_uid = x->uid;
	st->st_gid = x->gid;
	st->st_ino = x->ino;
	st->st_size = (off_t)x->size;
	st->st_dev = (int32_t)(x->dev_major << 8 | x->dev_minor);
	st->st_mtimespec.tv_sec = (long)x->mtime.sec;
	st->st_mtimespec.tv_nsec = (long)x->mtime.nsec;
}

/*
 * A race the test plays out: the next lstat of SWAP_AT sees what is there (or that nothing is), and then, before the
 * program's next call, the name is made a symbolic link to SWAP_TO (a host path) -- as another program could
 */
static const char *swap_at = "", *swap_to = "";

int
t_lstat(const char *p, struct stat *st)
{
	struct host_statx x;
	int result = answer(statx(HOST_AT_FDCWD, host_path(p), HOST_AT_SYMLINK_NOFOLLOW, HOST_STATX_BASIC, &x));

	if (result == 0) {
		from_statx(&x, st);
	}
	if (swap_at[0] != 0 && strcmp(p, swap_at) == 0) {
		char h[4096], command[4200];

		snprintf(h, sizeof(h), "%s", host_path(p));
		snprintf(command, sizeof(command), "rm -rf '%s'", h);
		(void)system(command);
		if (symlink(swap_to, h) != 0) {
			printf("FAIL: %s cannot be swapped for a link\n", h);
			failures++;
		}
		swap_at = "";
	}
	return result == 0 ? 0 : -1;
}

int
t_fstat(int f, struct stat *st)
{
	struct host_statx x;

	if (answer(statx(f, "", HOST_AT_EMPTY_PATH, HOST_STATX_BASIC, &x)) != 0) {
		return -1;
	}
	from_statx(&x, st);
	return 0;
}

int t_mkdir(const char *p, int mode) { return answer(mkdir(host_path(p), (unsigned)mode)); }
/* chown() follows a symbolic link: one given to a link sets the owner of whatever it leads to */
static unsigned chowned_through_links;

int
t_chown(const char *p, int uid, int gid)
{
	struct host_statx x;

	if (statx(HOST_AT_FDCWD, host_path(p), HOST_AT_SYMLINK_NOFOLLOW, HOST_STATX_BASIC, &x) == 0 && (x.mode & 0170000) == 0120000) {
		chowned_through_links++;
	}
	return answer(chown(host_path(p), (unsigned)uid, (unsigned)gid));
}
int t_lchown(const char *p, int uid, int gid) { return answer(lchown(host_path(p), (unsigned)uid, (unsigned)gid)); }
int t_chmod(const char *p, int mode) { return answer(chmod(host_path(p), (unsigned)mode)); }
int t_unlink(const char *p) { return answer(unlink(host_path(p))); }
int t_symlink(const char *target, const char *p) { return answer(symlink(target, host_path(p))); }

int
t_link(const char *from, const char *to)
{
	char f[4096];

	snprintf(f, sizeof(f), "%s", host_path(from));
	return answer(link(f, host_path(to)));
}

int
t_utimes(const char *p, const struct timeval times[2])
{
	struct timespec ts[2];

	for (int i = 0; i < 2; i++) {
		ts[i].tv_sec = times[i].tv_sec;
		ts[i].tv_nsec = (long)times[i].tv_usec * 1000;
	}
	return answer(utimensat(HOST_AT_FDCWD, host_path(p), ts, 0));
}

/*
 * The extended attributes: FAKE_XATTRS of them on FAKE_XATTR_PATH, names of two characters, each value one byte; and on
 * FORGED_ACL_PATH one alone, named as the archive names the access control list (anyone who can write a file can set it)
 */
static const char *fake_xattr_path = "", *forged_acl_path = "";
static unsigned fake_xattrs;

ssize_t
t_listxattr(const char *p, char *names, size_t size, int options)
{
	size_t used = 0;

	(void)options;
	if (strcmp(p, forged_acl_path) == 0 && size > sizeof(ACL_NAME)) {
		memcpy(names, ACL_NAME, sizeof(ACL_NAME));
		return (ssize_t)sizeof(ACL_NAME);
	}
	if (strcmp(p, fake_xattr_path) != 0) {
		return 0;
	}
	for (unsigned i = 0; i < fake_xattrs; i++) {
		if (used + 3 > size) {
			var_store_errno = 34;   /* ERANGE */
			return -1;
		}
		names[used++] = (char)('!' + i / 94);
		names[used++] = (char)('!' + i % 94);
		names[used++] = 0;
	}
	return (ssize_t)used;
}

ssize_t
t_getxattr(const char *p, const char *name, void *value, size_t size, uint32_t position, int options)
{
	(void)name;
	(void)position;
	(void)options;
	if ((strcmp(p, fake_xattr_path) != 0 && strcmp(p, forged_acl_path) != 0) || size < 1) {
		return -1;
	}
	*(uint8_t *)value = 'v';
	return 1;
}

int
t_setxattr(const char *p, const char *name, const void *value, size_t size, uint32_t position, int options)
{
	(void)p;
	(void)name;
	(void)value;
	(void)size;
	(void)position;
	(void)options;
	return 0;
}

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen)
{
	(void)newp;
	(void)newlen;
	if (strcmp(name, "kern.bootargs") == 0 && oldp != NULL && oldlenp != NULL) {
		size_t n = strlen(boot_line);

		if (n > *oldlenp) {
			n = *oldlenp;
		}
		memcpy(oldp, boot_line, n);
		*oldlenp = n;
		return 0;
	}
	return -1;
}

int
chflags(const char *p, int flags)
{
	(void)p;
	(void)flags;
	return 0;
}

/* the access control list: FAKE_ACL_PATH has one of four bytes, nothing else any */
static const char *fake_acl_path = "";

int
lstat_extended(const char *p, struct stat *st, void *sec, size_t *size)
{
	(void)st;
	if (strcmp(p, fake_acl_path) != 0 || *size < 4) {
		return -1;
	}
	memset(sec, 0xac, 4);
	*size = 4;
	return 0;
}

int
chmod_extended(const char *p, uint32_t uid, uint32_t gid, int mode, const void *sec)
{
	(void)p;
	(void)uid;
	(void)gid;
	(void)mode;
	(void)sec;
	return -1;
}

static uint8_t test_buffer[BUFFER];
static uint8_t link_space[LINKS * LINK_PATH * 2];       /* the hard links' names (links) */

/* ---- archives of the test's making, through the program's own stream ---- */

static void
archive_begin(int slot)
{
	stream_start(slot, 0);
	entries = 0;
	memset(links, 0, LINKS * sizeof(*links));
}

static void
archive_entry(uint32_t kind, uint32_t mode, const char *relative, const void *data, uint64_t size, int64_t mtime)
{
	struct entry e;

	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.mode = mode;
	e.uid = e.gid = (uint32_t)getuid();
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = mtime;
	e.size = size;
	CHECK(out(&e, sizeof(e)) && out(relative, e.pathlen) && (size == 0 || out(data, size)) && out_pad(), "%s packed", relative);
	entries++;
}

/* The archive's tail and its header, as save() writes them. */
static void
archive_end(int slot, uint64_t generation)
{
	static uint8_t block[BLOCK];
	struct head h;

	CHECK(out_flush(1), "slot %d: archive flushed", slot);
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = generation;
	h.bytes = stream_bytes;
	h.entries = entries;
	h.crc = stream_crc ^ 0xffffffffu;
	h.head_crc = head_checksum(&h);
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	CHECK(device_write(slot_at(slot), block, BLOCK), "slot %d: header written", slot);
}

/* A host file's contents, as a string (empty if there is none). */
static const char *
host_file(const char *p)
{
	static char text[256];
	int f = open(p, 0, 0);
	ssize_t n = f < 0 ? 0 : read(f, text, sizeof(text) - 1);

	if (f >= 0) {
		close(f);
	}
	text[n > 0 ? n : 0] = 0;
	return text;
}

static void
host_put(const char *p, const char *text)
{
	int f = open(p, 1 | HOST_O_CREAT | HOST_O_TRUNC, 0644);

	CHECK(f >= 0 && write(f, text, strlen(text)) == (ssize_t)strlen(text), "%s written", p);
	if (f >= 0) {
		close(f);
	}
}

static int
host_statx_of(const char *p, struct host_statx *x)
{
	return statx(HOST_AT_FDCWD, p, HOST_AT_SYMLINK_NOFOLLOW, HOST_STATX_BASIC, x);
}

/* A slot as save() leaves it: BYTES of archive, every byte FILL, and a header that checks out. */
static void
put_slot(int slot, uint64_t bytes, uint8_t fill, uint64_t generation)
{
	static uint8_t block[BLOCK];
	struct head h;

	memset(block, fill, sizeof(block));
	for (uint64_t done = 0; done < bytes; done += BLOCK) {
		CHECK(device_write(slot_at(slot) + BLOCK + (off_t)done, block, BLOCK), "slot %d: archive written", slot);
	}
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = generation;
	h.bytes = bytes;
	h.entries = 1;
	h.head_crc = head_checksum(&h);
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	CHECK(device_write(slot_at(slot), block, BLOCK), "slot %d: header written", slot);
}

/* How many bytes from WHERE on are not zero. */
static uint64_t
not_zero(off_t where, uint64_t n)
{
	static uint8_t block[BLOCK];
	uint64_t count = 0;

	while (n > 0) {
		uint64_t piece = n > BLOCK ? BLOCK : n;

		if (!device_read(where, block, piece)) {
			return n;
		}
		for (uint64_t i = 0; i < piece; i++) {
			count += block[i] != 0;
		}
		where += (off_t)piece;
		n -= piece;
	}
	return count;
}

int
main(void)
{
	static char name[256];
	const char *tmp = getenv("TMPDIR");
	struct head h;
	size_t n;

	crc_init();
	buffer = test_buffer;
	n = strlen(tmp != NULL && *tmp != 0 ? tmp : "/tmp");
	memcpy(name, tmp != NULL && *tmp != 0 ? tmp : "/tmp", n);
	memcpy(name + n, "/var_store_test.", sizeof("/var_store_test."));
	n += sizeof("/var_store_test.") - 1;
	for (unsigned pid = (unsigned)getpid(), div = 1000000000u; div > 0; div /= 10) {
		name[n++] = (char)('0' + pid / div % 10);       /* (one file a run: runs side by side do not meet) */
	}
	name[n] = 0;
	unlink(name);
	fd = open(name, O_RDWR | HOST_O_CREAT, 0600);
	if (fd < 0) {
		printf("var_store_test: %s cannot be made\n", name);
		return 1;
	}
	/* (as long as the partition, sparse: a slot is read a bufferful at a time, past its archive) */
	CHECK(ftruncate(fd, STORE_AT + SLOTS * SLOT_BYTES) == 0, "the partition's file is as long as the slots");

	/* erase: an archive that does not end on a block (10000 bytes), one of whole blocks, and what lies past each */
	put_slot(0, 10000, 0xa5, 7);
	put_slot(1, 3 * BLOCK, 0x5a, 8);
	{
		static uint8_t past[BLOCK];

		memset(past, 0x77, sizeof(past));
		CHECK(device_write(slot_at(0) + BLOCK + 3 * BLOCK, past, BLOCK), "past slot 0's archive");
		CHECK(device_write(slot_at(1) + BLOCK + 3 * BLOCK, past, BLOCK), "past slot 1's archive");
	}
	CHECK(read_head(0, &h) && read_head(1, &h), "both slots check out before the erase");
	CHECK(not_zero(slot_at(0) + BLOCK, 10000) == 10000, "slot 0 holds its archive");
	CHECK(erase(), "the erase succeeds");
	CHECK(!read_head(0, &h) && !read_head(1, &h), "neither slot checks out after it");
	CHECK(not_zero(slot_at(0), BLOCK) == 0 && not_zero(slot_at(1), BLOCK) == 0, "both headers are zero");
	CHECK(not_zero(slot_at(0) + BLOCK, 3 * BLOCK) == 0, "slot 0's archive is zero, its last part block too: %llu bytes are not",
	    (unsigned long long)not_zero(slot_at(0) + BLOCK, 3 * BLOCK));
	CHECK(not_zero(slot_at(1) + BLOCK, 3 * BLOCK) == 0, "slot 1's archive is zero: %llu bytes are not",
	    (unsigned long long)not_zero(slot_at(1) + BLOCK, 3 * BLOCK));
	CHECK(not_zero(slot_at(0) + 4 * BLOCK, BLOCK) == BLOCK && not_zero(slot_at(1) + 4 * BLOCK, BLOCK) == BLOCK,
	    "nothing past what the header said is written");
	CHECK(erase(), "an erase of an erased store succeeds, and writes the headers alone");

	/* a header that says more than a slot holds does not check out, and nothing is written for it */
	put_slot(0, BLOCK, 0x11, 9);
	{
		static uint8_t block[BLOCK];
		struct head *bad = (struct head *)block;

		CHECK(device_read(slot_at(0), block, BLOCK), "header read");
		bad->bytes = (uint64_t)SLOT_BYTES;
		bad->head_crc = head_checksum(bad);
		CHECK(device_write(slot_at(0), block, BLOCK), "header written");
		CHECK(!read_head(0, &h), "a header that says more than the slot holds is not one");
		CHECK(erase(), "the erase succeeds");
		CHECK(not_zero(slot_at(0) + BLOCK, BLOCK) == BLOCK, "and goes no further than the header");
	}

	/* the keychain: left out unless the boot line asks for it, and only that name */
	boot_line = "var_store=1 ufs_write=1";
	keychain_wanted = -1;
	CHECK(is_skipped("Keychains"), "the keychain is left out by default");
	CHECK(!is_skipped("Keychains2") && !is_skipped("mobile/Keychains"), "and nothing else by that name");
	boot_line = "var_store=1 var_store_keychain=10";
	keychain_wanted = -1;
	CHECK(is_skipped("Keychains"), "var_store_keychain=10 is not var_store_keychain=1");
	boot_line = "var_store=1 var_store_keychain=1 ufs_write=1";
	keychain_wanted = -1;
	CHECK(!is_skipped("Keychains"), "var_store_keychain=1 keeps it");
	CHECK(is_skipped("tmp"), "the rest of SKIP stays left out");

	/* ---- what load makes of an archive, in a /private/var of the test's ---- */
	boot_line = "var_store=1";
	keychain_wanted = -1;
	links = (void *)link_space;
	snprintf(test_root, sizeof(test_root), "%s.d", name);
	{
		char p[1024];

		snprintf(p, sizeof(p), "rm -rf '%s'", test_root);
		(void)system(p);
		CHECK(mkdir(test_root, 0755) == 0, "%s made", test_root);
		snprintf(p, sizeof(p), "%s/var", test_root);
		CHECK(mkdir(p, 0755) == 0, "%s made", p);
		snprintf(p, sizeof(p), "%s/outside", test_root);
		CHECK(mkdir(p, 0751) == 0, "%s made", p);
	}

	/*
	 * xattr_count_load_reads: a file of anyone's with more extended attributes than load reads of one entry (4096) --
	 * thousands of tiny ones, which the memory filesystem takes -- is packed with no more than that: else the whole
	 * archive is one load refuses, and every file of the volume is lost at the next boot for the one
	 */
	fake_xattr_path = ROOT "/many";
	fake_acl_path = ROOT "/many";           /* (and an access control list, which is one of them in the archive) */
	fake_xattrs = 4200;
	strcpy(path, fake_xattr_path);
	{
		uint32_t count = 0;
		const size_t namelen = strlen(ACL_NAME);
		uint64_t used = pack_xattrs(&count);

		CHECK(count == 4096, "as many as load reads of one entry, and no fewer: %u", count);
		CHECK(used >= 8 + namelen + 4 && memcmp(xpack + used - 4 - namelen, ACL_NAME, namelen) == 0,
		    "the access control list among them, not left out for the others");
	}
	{
		struct stat st;

		memset(&st, 0, sizeof(st));
		st.st_mode = S_IFDIR | 0755;
		archive_begin(0);
		CHECK(pack_one("many", &st), "a directory with 4200 attributes is packed");
		archive_end(0, 50);
		CHECK(read_head(0, &h) && pass(&h, 0), "and the archive it is in reads back whole (load would take it)");
	}
	fake_xattrs = 0;
	fake_acl_path = "";

	/*
	 * acl_attribute_not_taken: an extended attribute named as the archive names the access control list, set by whoever
	 * can write the file, is not packed: load, as root, would install it as the file's list
	 */
	forged_acl_path = ROOT "/forged";
	strcpy(path, forged_acl_path);
	{
		uint32_t count = 1;

		CHECK(pack_xattrs(&count) == 0 && count == 0, "the forged list is left out: %u attributes", count);
	}
	forged_acl_path = "";

	/*
	 * hardlink_first_name_unpacked: an inode of two names whose first name was not packed after all -- swapped for
	 * another file between the walk's lstat and the open (an atomic save over it), or gone -- has its other name
	 * packed whole, not as a link to a name the archive does not hold: restored as such a link, it was nothing at all
	 */
	{
		char p[1024], q[1024];
		struct stat st;

		snprintf(p, sizeof(p), "%s/var/second", test_root);
		host_put(p, "the contents");
		CHECK(t_lstat(ROOT "/second", &st) == 0, "the file is there");
		st.st_nlink = 2;                /* (its other name, first in the walk, is gone by the time it is opened) */
		archive_begin(1);
		strcpy(path, ROOT "/first");
		CHECK(pack_one("first", &st), "the first name, gone, is passed over");
		strcpy(path, ROOT "/second");
		CHECK(pack_one("second", &st), "the second name is packed");
		archive_end(1, 51);
		unlink(p);                      /* (a new boot's /private/var) */
		CHECK(load(), "the store is loaded");
		snprintf(q, sizeof(q), "%s", host_file(p));
		CHECK(strcmp(q, "the contents") == 0, "the second name comes back with its contents: \"%s\"", q);
	}

	/*
	 * restore_through_symlink: what init made a symbolic link of before the store is unpacked (/private/var/jb leads
	 * into /private/preboot with the root on the flash) is not unpacked through: a directory of the archive by that
	 * name gives its owner, mode, times and files to nothing outside /private/var, and a file of the archive replaces
	 * the link rather than writing where it leads
	 */
	{
		char p[1024], q[1024], text[256];
		struct host_statx x;

		snprintf(q, sizeof(q), "%s/outside", test_root);
		snprintf(p, sizeof(p), "%s/var/jb", test_root);
		CHECK(symlink(q, p) == 0, "jb leads outside");
		snprintf(p, sizeof(p), "%s/var/made", test_root);
		CHECK(symlink(q, p) == 0, "made leads outside");
		snprintf(q, sizeof(q), "%s/outside/target", test_root);
		host_put(q, "outside");
		snprintf(p, sizeof(p), "%s/var/file", test_root);
		CHECK(symlink(q, p) == 0, "file leads outside");
		archive_begin(0);
		archive_entry(ENT_DIR, 0700, "jb", NULL, 0, 0);
		archive_entry(ENT_DIRTIME, 0, "jb", NULL, 0, 1000000);
		archive_entry(ENT_DIR, 0700, "made", NULL, 0, 0);
		archive_entry(ENT_FILE, 0600, "made/new", "store", 5, 1000000);
		archive_entry(ENT_DIRTIME, 0, "made", NULL, 0, 1000000);
		archive_entry(ENT_FILE, 0600, "file", "store", 5, 1000000);
		archive_end(0, 52);
		CHECK(load(), "the store is loaded");
		snprintf(q, sizeof(q), "%s/outside", test_root);
		CHECK(host_statx_of(q, &x) == 0 && (x.mode & 07777) == 0751, "the directory a link leads to keeps its mode: %o",
		    x.mode & 07777);
		CHECK(x.mtime.sec != 1000000, "and its time");
		snprintf(q, sizeof(q), "%s/outside/new", test_root);
		CHECK(host_statx_of(q, &x) != 0, "nothing of the archive is made in it");
		snprintf(q, sizeof(q), "%s/outside/target", test_root);
		snprintf(text, sizeof(text), "%s", host_file(q));
		CHECK(strcmp(text, "outside") == 0, "the file a link leads to is not written: \"%s\"", text);
		snprintf(p, sizeof(p), "%s/var/file", test_root);
		snprintf(text, sizeof(text), "%s", host_file(p));
		CHECK(host_statx_of(p, &x) == 0 && (x.mode & 0170000) == 0100000 && strcmp(text, "store") == 0,
		    "the archive's file is there in the link's place: \"%s\"", text);
	}

	/*
	 * hardlink_packed_as_link: an inode of two names, both packed, is packed once: its second name as a link to the first,
	 * and it comes back one inode, not two copies
	 */
	{
		char p[1024], q[1024];
		struct stat st;
		struct host_statx x, y;

		snprintf(p, sizeof(p), "%s/var/hl_first", test_root);
		snprintf(q, sizeof(q), "%s/var/hl_second", test_root);
		host_put(p, "one inode");
		CHECK(link(p, q) == 0, "its second name");
		archive_begin(0);
		strcpy(path, ROOT "/hl_first");
		CHECK(t_lstat(path, &st) == 0 && st.st_nlink == 2 && pack_one("hl_first", &st), "the first name is packed");
		strcpy(path, ROOT "/hl_second");
		CHECK(t_lstat(path, &st) == 0 && pack_one("hl_second", &st), "the second name is packed");
		archive_end(0, 53);
		unlink(p);
		unlink(q);
		CHECK(load(), "the store is loaded");
		CHECK(host_statx_of(p, &x) == 0 && host_statx_of(q, &y) == 0 && x.ino == y.ino && x.nlink == 2,
		    "the two names are one inode again");
		CHECK(strcmp(host_file(q), "one inode") == 0, "with its contents");
	}

	/*
	 * swapped_before_open: a file another program replaced with a file of its own between the walk's lstat and the open
	 * (a rename over it) is left out: packed, the newcomer's contents would be restored with the first one's owner and mode
	 */
	{
		char p[1024], q[1024];
		struct stat st;

		snprintf(p, sizeof(p), "%s/var/swapped", test_root);
		snprintf(q, sizeof(q), "%s/var/swapped.new", test_root);
		host_put(p, "looked at");
		strcpy(path, ROOT "/swapped");
		CHECK(t_lstat(path, &st) == 0, "the file is looked at");
		host_put(q, "put in its place");
		CHECK(rename(q, p) == 0, "and replaced");
		archive_begin(1);
		changed_under = 0;
		CHECK(pack_one("swapped", &st) && changed_under == 1 && entries == 0,
		    "it is left out and counted: %u changed, %llu entries", changed_under, (unsigned long long)entries);
		unlink(p);
	}

	/*
	 * walk_not_led_out: a directory made a symbolic link to a directory outside /private/var between the walk's lstat
	 * and its open is not walked into: nothing of where it leads goes into the archive
	 */
	{
		char p[1024], q[1024];

		snprintf(p, sizeof(p), "%s/var/walked", test_root);
		snprintf(q, sizeof(q), "%s/outside", test_root);
		CHECK(mkdir(p, 0755) == 0, "%s made", p);
		archive_begin(1);
		swap_at = ROOT "/walked";
		swap_to = q;
		strcpy(path, ROOT "/walked");
		CHECK(pack_tree("walked") && entries == 1, "the directory's own entry, nothing of outside: %llu entries",
		    (unsigned long long)entries);
		swap_at = "";
		unlink(p);
	}

	/*
	 * restore_raced_by_a_link: a name the archive has a file for, made a symbolic link out of /private/var between load's
	 * lstat and its open, is not written through, nor given the file's mode
	 */
	{
		char q[1024], text[256];
		struct host_statx x;

		snprintf(q, sizeof(q), "%s/outside/target", test_root);
		archive_begin(0);
		archive_entry(ENT_FILE, 0600, "raced", "store", 5, 1000000);
		archive_end(0, 54);
		swap_at = ROOT "/raced";
		swap_to = q;
		CHECK(load(), "the store is loaded");
		swap_at = "";
		snprintf(text, sizeof(text), "%s", host_file(q));
		CHECK(strcmp(text, "outside") == 0, "the file the link leads to is not written: \"%s\"", text);
		CHECK(host_statx_of(q, &x) == 0 && (x.mode & 07777) == 0644, "nor given the archive's mode: %o", x.mode & 07777);
	}

	/*
	 * restore_links_and_names: a symbolic link of the archive replaces the one the skeleton made by its name, and is
	 * given its owner without following it; a file of the archive where a directory is gives the directory nothing; a
	 * name that climbs out of /private/var is made nowhere
	 */
	{
		char p[1024], q[1024], target[256];
		struct host_statx x;
		ssize_t n;

		snprintf(p, sizeof(p), "%s/var/lnk", test_root);
		snprintf(q, sizeof(q), "%s/outside", test_root);
		CHECK(symlink(q, p) == 0, "the skeleton's link");
		snprintf(p, sizeof(p), "%s/var/adir", test_root);
		CHECK(mkdir(p, 0751) == 0, "%s made", p);
		archive_begin(0);
		archive_entry(ENT_SYMLINK, 0755, "lnk", "store/target", 12, 1000000);
		archive_entry(ENT_FILE, 0600, "adir", "store", 5, 1000000);
		archive_entry(ENT_FILE, 0600, "../escaped", "store", 5, 1000000);
		archive_end(0, 55);
		chowned_through_links = 0;
		CHECK(load(), "the store is loaded");
		snprintf(p, sizeof(p), "%s/var/lnk", test_root);
		n = readlink(p, target, sizeof(target) - 1);
		target[n > 0 ? n : 0] = 0;
		CHECK(strcmp(target, "store/target") == 0, "the archive's link in the skeleton's place: \"%s\"", target);
		CHECK(chowned_through_links == 0, "no owner set through a link: %u", chowned_through_links);
		snprintf(p, sizeof(p), "%s/var/adir", test_root);
		CHECK(host_statx_of(p, &x) == 0 && (x.mode & 0170000) == 0040000 && (x.mode & 07777) == 0751,
		    "the directory keeps its mode: %o", x.mode & 07777);
		snprintf(p, sizeof(p), "%s/escaped", test_root);
		CHECK(host_statx_of(p, &x) != 0, "nothing made outside /private/var");
	}
	/*
	 * save_keeps_the_good_copy: the newest slot's header checks out but its archive does not (a save torn after the
	 * header reached the flash): load took the slot before it, and the next save writes over the torn one, not over the
	 * only good copy there is; with both whole, the older is written, as always
	 */
	{
		static uint8_t junk[BLOCK];
		struct head h1, old;
		int best = -2;

		archive_begin(0);
		archive_entry(ENT_FILE, 0600, "kept", "good", 4, 1000000);
		archive_end(0, 60);
		archive_begin(1);
		archive_entry(ENT_FILE, 0600, "newer", "newer", 5, 1000000);
		archive_end(1, 61);
		good_slot = -1;                 /* (a new process: nothing read through yet) */
		memset(&old, 0, sizeof(old));
		CHECK(save_slot(&old, &best) == 0 && best == 1, "both whole: the older slot is written (best %d)", best);
		memset(junk, 0x5c, sizeof(junk));
		CHECK(device_write(slot_at(1) + BLOCK, junk, BLOCK), "slot 1's archive torn under its header");
		CHECK(read_head(1, &h1) && !pass(&h1, 0), "slot 1's header checks out, its archive does not");
		good_slot = -1;
		memset(&old, 0, sizeof(old));
		CHECK(save_slot(&old, &best) == 1 && best == 1 && old.generation == 61,
		    "the torn slot is the one written, slot 0, the good copy, kept (best %d)", best);
	}
	/*
	 * whole_by_generation: what archive_whole remembers is a slot AND its generation: another archive in that slot later
	 * -- torn -- is read through again, not taken for the whole one that was there
	 */
	{
		static uint8_t junk[BLOCK];
		struct head h0;

		good_slot = -1;
		archive_begin(0);
		archive_entry(ENT_FILE, 0600, "kept", "good", 4, 1000000);
		archive_end(0, 62);
		CHECK(read_head(0, &h0) && archive_whole(0, &h0), "generation 62 in slot 0 reads back whole");
		archive_begin(0);
		archive_entry(ENT_FILE, 0600, "kept", "newer", 5, 1000000);
		archive_end(0, 64);
		memset(junk, 0x5c, sizeof(junk));
		CHECK(device_write(slot_at(0) + BLOCK, junk, BLOCK), "slot 0's archive torn under its header");
		CHECK(read_head(0, &h0) && h0.generation == 64 && !archive_whole(0, &h0),
		    "generation 64 in the same slot, torn, is not taken for the whole one remembered");
	}
	{
		char p[1024];

		snprintf(p, sizeof(p), "rm -rf '%s'", test_root);
		(void)system(p);
	}

	close(fd);
	unlink(name);
	printf("var_store_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
