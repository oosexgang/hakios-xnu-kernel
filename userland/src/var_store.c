/*
 * The data volume of iOS between two boots: /private/var packed into the phone's flash and
 * unpacked out of it again.
 *
 * /private/var is a memory filesystem (bsd/miscfs/ramfs), mounted empty by init at every boot and
 * given the directories of the template iOS ships with (ios165/mkvarskel.py), so nothing iOS keeps
 * there outlives the boot that wrote it: no home screen layout, no application data, no baked
 * wallpaper. The kernel's APFS driver reads and does not write, and writing APFS is not the point
 * here -- what is needed is that the bytes come back. So the tree is packed into a plain archive
 * and the archive is written into the one place on the phone's flash this port owns and nothing of
 * it uses: the partition darwin, past the iOS volume and both cryptexes. The kernel takes those
 * writes on /dev/rufs_darwin alone and refuses anything else (bsd/dev/redwood_ufs.c, ufs_write=1).
 *
 *   var_store save          pack /private/var into the store
 *   var_store load          unpack the newest good store into /private/var (init, at boot)
 *   var_store watch [SECS]  stay and save by itself: when the kernel says the run is ending, and
 *                           every SECS seconds besides (a launchd job of ours does this)
 *   var_store show          what the store holds, both slots
 *   var_store erase         forget both slots (the next boot starts empty again)
 *   var_store stamp PATH    write a line into PATH saying when: a marker for a run
 *
 * Nobody has to remember to save. `watch` registers with the kernel (kern.redwood.var_save), and
 * on the orderly way back to the loader -- the run's time up, or "xl stop" -- the kernel asks it
 * for the volume and waits, bounded, before it goes (bsd/dev/redwood_ufs.c,
 * redwood_var_save_on_return). A save every SECS seconds is there as well but is not the default:
 * one costs seconds of the run and a hundred-odd megabytes of flash, and while it runs the
 * storage is read through the loader rather than through this kernel's own driver.
 *
 * One saver at a time: a write to the store is refused unless the writer holds the claim
 * (kern.redwood.var_busy), so two of them cannot interleave into one slot. Even if they did, the
 * slot being written is never the one holding the newest good archive, and a header is written
 * last and over a zeroed one -- so the worst an interrupted or doubled save can do is lose itself.
 *
 * Two slots, written in turn: the one not holding the newest good archive is the one written, its
 * header first zeroed, the archive next, the header last. So a save that does not finish -- the
 * phone reset in the middle of it -- loses nothing: the other slot still holds the archive from
 * before, and `load` takes the newest slot whose header and contents both check out.
 *
 * `load` reads the archive twice: once to see that its checksum is the one in the header, and only
 * then to make files. A store that was torn or is half a different archive therefore leaves
 * /private/var as init made it rather than half filled.
 *
 * What is left out: sockets and devices, which the daemons of a new boot make for themselves, and
 * the directories in SKIP below, which hold what only this boot can mean (its runtime sockets, its
 * swap, its logs) or what is grafted from a read-only image at every boot anyway.
 */
#include "mini.h"

#define BLOCK           4096
#define BUFFER          (4u << 20)              /* the loader's own buffer: one request a write */
#define STORE_AT        8589934592LL            /* darwin's block 2097152: past the cryptexes, with room to spare */
#define SLOT_BYTES      0x80000000LL            /* 2 GiB a slot, two slots */
#define SLOTS           2
#define DEVICE          "/dev/rufs_darwin"
#define ROOT            "/private/var"
#define MAX_PATH        1024
#define MAX_DEPTH       32
#define MAX_XATTR       (256u << 10)            /* one attribute's value; iOS keeps small ones */
#define XPACK           (512u << 10)            /* all the attributes of one file, packed */
#define LINKS           8192                    /* names remembered for hard links */
#define LINK_PATH       256
/*
 * The file flags put back: the ones a tree of files can mean. UF_COMPRESSED is not among them
 * (a ramfs file is not a compressed HFS one, and the flag would make it read as empty), nor are
 * the flags only a single-user boot can clear.
 */
#define FLAGS_KEPT      (0x1u | 0x2u | 0x4u | 0x8u | 0x8000u)   /* nodump, immutable, append, opaque, hidden */

/* the directories whose contents are not kept (the directory itself is) */
static const char *const SKIP[] = {
	"tmp",                  /* its name says it */
	"run",                  /* runtime sockets and pid files: this boot's, not the last one's */
	"vm",                   /* the compressor's swap */
	"log", "logs",          /* what the last boot said */
	"db/diagnostics", "db/uuidtext",        /* logd's archive: tens of MB a boot, and remade */
	"jb",                   /* the package prefix, grafted from the read-only image at every boot */
	NULL,
};

enum { ENT_DIR = 1, ENT_FILE = 2, ENT_SYMLINK = 3, ENT_HARDLINK = 4, ENT_DIRTIME = 5 };

#define HEAD_MAGIC      "RDWVAR\0"
#define HEAD_VERSION    1

struct head {
	char     magic[8];
	uint32_t version;
	uint32_t slot;
	uint64_t generation;            /* the newest good slot is the one with the highest */
	uint64_t bytes;                 /* the archive, without the padding to a whole block */
	uint64_t entries;
	uint64_t seconds;               /* when it was written */
	uint32_t crc;                   /* of those bytes */
	uint32_t head_crc;              /* of this structure with head_crc zero */
};

struct entry {
	uint32_t kind;
	uint32_t mode;                  /* permission bits */
	uint32_t uid, gid;
	uint32_t flags;                 /* st_flags */
	uint32_t pathlen;
	uint32_t nxattr;
	uint32_t mtime_nsec;
	int64_t  mtime_sec;
	uint64_t size;                  /* a file's contents, a link's target, a hard link's first name */
};

/* ---- the kernel's side of it: the boot-arg, the handshake, the claim ---- */

#define VAR_SAVE_NONE   0
#define VAR_SAVE_IDLE   1
#define VAR_SAVE_WANTED 2
#define VAR_SAVE_DONE   3

/* Is NAME=VALUE among the kernel's arguments? (as init.c reads them) */
static int
boot_arg_is(const char *name, const char *value)
{
	static char args[1024];
	size_t len = sizeof(args) - 1, n = strlen(name), v = strlen(value);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=' && strncmp(p + n + 1, value, v) == 0 &&
		    (p[n + 1 + v] == 0 || p[n + 1 + v] == ' ')) {
			return 1;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return 0;
}

static long
parse_number(const char *text)
{
	long value = 0;

	for (; *text >= '0' && *text <= '9'; text++) {
		value = value * 10 + (*text - '0');
	}
	return value;
}

static int
sysctl_int(const char *name, int *value)
{
	size_t len = sizeof(*value);

	return sysctlbyname(name, value, &len, NULL, 0);
}

static int
sysctl_set_int(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value));
}

/* The claim on the store: nothing may be written without it, and only one program may hold it. */
static int
claim(void)
{
	if (sysctl_set_int("kern.redwood.var_busy", 1) != 0) {
		printf("var_store: the store is claimed by another saver (%s)\n", strerror(errno));
		return 0;
	}
	return 1;
}

static void
release(void)
{
	(void)sysctl_set_int("kern.redwood.var_busy", 0);
}

/* ---- checksum (the same one the loader reads a write back with) ---- */

static uint32_t crc_table[256];

static void
crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
		crc_table[i] = c;
	}
}

static uint32_t
crc_add(uint32_t c, const void *p, uint64_t n)
{
	const uint8_t *b = p;

	for (uint64_t i = 0; i < n; i++) {
		c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
	}
	return c;
}

static uint32_t
head_checksum(const struct head *h)
{
	struct head copy = *h;

	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

/* ---- the store, as a stream of bytes in one slot ---- */

static int fd = -1;
static uint8_t *buffer;
static uint8_t *already;                /* what the slot holds there now, to not write it again */
static uint64_t skipped_blocks, written_blocks;
static uint64_t buffered;               /* bytes in the buffer: to be written, or still to be read */
static uint64_t taken;                  /* of those, already read out (load) */
static off_t at;                        /* where in the device the buffer begins */
static uint64_t stream_bytes;           /* the archive's own bytes so far */
static uint64_t stream_left;            /* bytes of the archive still to come (load) */
static off_t stream_limit;              /* the end of the slot: nothing is read or written past it */
static uint32_t stream_crc;
static uint64_t entries;

static off_t
slot_at(int slot)
{
	return STORE_AT + (off_t)slot * SLOT_BYTES;
}

static int
device_write(off_t where, const void *data, uint64_t n)
{
	const uint8_t *p = data;

	while (n > 0) {
		ssize_t wrote = pwrite(fd, p, (size_t)n, where);

		if (wrote <= 0) {
			printf("var_store: writing %llu bytes at %lld: %s\n", (unsigned long long)n,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += wrote;
		where += wrote;
		n -= (uint64_t)wrote;
	}
	return 1;
}

static int
device_read(off_t where, void *data, uint64_t n)
{
	uint8_t *p = data;

	while (n > 0) {
		ssize_t got = pread(fd, p, (size_t)n, where);

		if (got <= 0) {
			printf("var_store: reading %llu bytes at %lld: %s\n", (unsigned long long)n,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += got;
		where += got;
		n -= (uint64_t)got;
	}
	return 1;
}

/* ---- writing the archive out ---- */

static int
out_flush(int last)
{
	uint64_t n = buffered;

	if (n == 0) {
		return 1;
	}
	if (last) {
		/* whole blocks only: the tail is padded, and the header says where the archive ends */
		while (n % BLOCK != 0) {
			buffer[n++] = 0;
		}
	}
	if (at + (off_t)n > stream_limit) {
		printf("var_store: the archive does not fit in its slot: nothing more is written\n");
		return 0;
	}
	/*
	 * What is already there is read first, and if it is what we were about to write, it is not
	 * written. A volume that iOS has stopped touching packs to the very same bytes -- measured:
	 * two saves a minute apart came out with the same checksum -- and the slots hold alternate
	 * copies of it, so a save that changes nothing then costs reads and no writes at all. Reads
	 * of this storage are several times cheaper than writes, and flash wears by writing.
	 */
	if (already != NULL && device_read(at, already, n) && memcmp(already, buffer, (size_t)n) == 0) {
		skipped_blocks += n / BLOCK;
	} else if (!device_write(at, buffer, n)) {
		return 0;
	} else {
		written_blocks += n / BLOCK;
	}
	at += (off_t)n;
	buffered = 0;
	return 1;
}

static int
out(const void *data, uint64_t n)
{
	const uint8_t *p = data;

	stream_crc = crc_add(stream_crc, data, n);
	stream_bytes += n;
	while (n > 0) {
		uint64_t room = BUFFER - buffered;

		if (room == 0) {
			if (!out_flush(0)) {
				return 0;
			}
			continue;
		}
		if (room > n) {
			room = n;
		}
		memcpy(buffer + buffered, p, (size_t)room);
		buffered += room;
		p += room;
		n -= room;
	}
	return 1;
}

static int
out_pad(void)
{
	static const uint8_t zero[8] = { 0 };
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : out(zero, 8 - over);
}

/* ---- reading the archive in ---- */

static int
in(void *data, uint64_t n)
{
	uint8_t *p = data;

	if (n > stream_left) {
		printf("var_store: the archive ends in the middle of an entry\n");
		return 0;
	}
	stream_left -= n;
	stream_bytes += n;
	while (n > 0) {
		uint64_t have = buffered - taken;

		if (have == 0) {
			uint64_t want = BUFFER;

			if (at + (off_t)want > stream_limit) {
				want = (uint64_t)(stream_limit - at);
			}
			if (want == 0 || !device_read(at, buffer, want)) {
				return 0;
			}
			at += (off_t)want;
			buffered = want;
			taken = 0;
			continue;
		}
		if (have > n) {
			have = n;
		}
		if (p != NULL) {
			memcpy(p, buffer + taken, (size_t)have);
			p += have;
		}
		stream_crc = crc_add(stream_crc, buffer + taken, have);
		taken += have;
		n -= have;
	}
	return 1;
}

/* n bytes of the archive passed over, their checksum counted in */
static int
in_skip(uint64_t n)
{
	uint8_t waste[512];

	while (n > 0) {
		uint64_t piece = n > sizeof(waste) ? sizeof(waste) : n;

		if (!in(waste, piece)) {
			return 0;
		}
		n -= piece;
	}
	return 1;
}

static int
in_pad(void)
{
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : in_skip(8 - over);
}

static void
stream_start(int slot, uint64_t bytes)
{
	at = slot_at(slot) + BLOCK;
	stream_limit = slot_at(slot) + SLOT_BYTES;
	buffered = taken = 0;
	stream_bytes = 0;
	stream_left = bytes;
	stream_crc = 0xffffffffu;
}

/* ---- the slots ---- */

/* The slot's header, if it is one of ours and says of itself what it should. */
static int
read_head(int slot, struct head *into)
{
	static uint8_t block[BLOCK];
	struct head *h = (struct head *)block;

	if (!device_read(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0 || h->version != HEAD_VERSION ||
	    h->slot != (uint32_t)slot || h->bytes > (uint64_t)SLOT_BYTES - BLOCK ||
	    h->head_crc != head_checksum(h)) {
		return 0;
	}
	*into = *h;
	return 1;
}

/* The newest slot whose header checks out, or -1. */
static int
newest(struct head *out_head)
{
	int best = -1;

	for (int slot = 0; slot < SLOTS; slot++) {
		struct head h;

		if (!read_head(slot, &h)) {
			continue;
		}
		if (best < 0 || h.generation > out_head->generation) {
			best = slot;
			*out_head = h;
		}
	}
	return best;
}

/* ---- packing ---- */

/*
 * How much of this save is new since the last one: an entry whose time of writing is later than
 * the second the previous generation was saved at. It costs nothing to count -- the walk has every
 * entry's time in hand anyway -- and it is what says whether saving only the difference would be
 * worth building, or whether iOS rewrites the volume faster than any such thing could help.
 */
static uint64_t last_saved_at, changed_entries, changed_bytes;

static char path[MAX_PATH];               /* the whole path, under ROOT */
static struct { uint64_t ino; char path[LINK_PATH]; } *links;
static uint64_t skipped_kinds, skipped_big;

static int
is_skipped(const char *relative)
{
	for (int i = 0; SKIP[i] != NULL; i++) {
		if (strcmp(relative, SKIP[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/* The name this inode was first met under, if it has one; otherwise this one is remembered. */
static const char *
link_name(uint64_t ino, const char *relative)
{
	uint64_t slot = (ino * 2654435761u) % LINKS;

	for (uint64_t tries = 0; tries < LINKS; tries++, slot = (slot + 1) % LINKS) {
		if (links[slot].ino == ino) {
			return links[slot].path;
		}
		if (links[slot].ino == 0) {
			if (strlen(relative) < LINK_PATH) {
				links[slot].ino = ino;
				strcpy(links[slot].path, relative);
			}
			return NULL;
		}
	}
	return NULL;                    /* the table is full: this one is written out whole */
}

/*
 * The extended attributes of path -- iOS keeps container metadata and much else in them -- packed
 * into a buffer of their own, because the entry's head says how many there are and has to go out
 * before them. One that does not fit is left out and counted.
 */
static uint8_t xpack[XPACK];

static uint64_t
pack_xattrs(uint32_t *count)
{
	static char names[16384];
	static uint8_t value[MAX_XATTR];
	ssize_t n = listxattr(path, names, sizeof(names), XATTR_NOFOLLOW);
	uint64_t used = 0;

	*count = 0;
	if (n <= 0) {
		return 0;
	}
	for (ssize_t i = 0; i < n && i < (ssize_t)sizeof(names); i += (ssize_t)strlen(names + i) + 1) {
		uint32_t namelen = (uint32_t)strlen(names + i), vlen;
		ssize_t size = getxattr(path, names + i, value, sizeof(value), 0, XATTR_NOFOLLOW);

		if (size < 0) {
			continue;
		}
		vlen = (uint32_t)size;
		if (used + 8 + namelen + vlen > XPACK) {
			skipped_big++;
			continue;
		}
		memcpy(xpack + used, &namelen, sizeof(namelen));
		memcpy(xpack + used + 4, &vlen, sizeof(vlen));
		memcpy(xpack + used + 8, names + i, namelen);
		memcpy(xpack + used + 8 + namelen, value, vlen);
		used += 8 + namelen + vlen;
		(*count)++;
	}
	return used;
}

/* One entry: its head, its name, its extended attributes, its contents. */
static int
pack_one(const char *relative, const struct stat *st)
{
	struct entry e;
	static char target[MAX_PATH];
	const char *first;
	uint32_t kind = 0;
	uint64_t size = 0;
	const void *tail = NULL;
	int in_fd = -1;

	switch (st->st_mode & S_IFMT) {
	case S_IFDIR:
		kind = ENT_DIR;
		break;
	case S_IFLNK: {
		ssize_t n = readlink(path, target, sizeof(target));

		if (n <= 0) {
			return 1;
		}
		kind = ENT_SYMLINK;
		size = (uint64_t)n;
		tail = target;
		break;
	}
	case S_IFREG:
		kind = ENT_FILE;
		size = (uint64_t)st->st_size;
		if (st->st_nlink > 1) {
			first = link_name(st->st_ino, relative);
			if (first != NULL) {
				kind = ENT_HARDLINK;
				size = strlen(first);
				tail = first;
				break;
			}
		}
		in_fd = open(path, O_RDONLY, 0);
		if (in_fd < 0) {
			return 1;       /* gone, or not ours to read: it is not in the archive */
		}
		break;
	default:
		skipped_kinds++;        /* a socket, a fifo, a device: a new boot makes its own */
		return 1;
	}

	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.mode = st->st_mode & 07777;
	e.uid = st->st_uid;
	e.gid = st->st_gid;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	e.size = size;
	if (last_saved_at != 0 && (uint64_t)st->st_mtimespec.tv_sec > last_saved_at) {
		changed_entries++;
		changed_bytes += size;
	}
	{
		uint64_t xlen = pack_xattrs(&e.nxattr);

		if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out(xpack, xlen)) {
			if (in_fd >= 0) {
				close(in_fd);
			}
			return 0;
		}
	}
	if (tail != NULL && !out(tail, size)) {
		if (in_fd >= 0) {
			close(in_fd);
		}
		return 0;
	}
	if (in_fd >= 0) {
		static uint8_t piece[256 * 1024];
		uint64_t left = size;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left;
			ssize_t got = read(in_fd, piece, (size_t)want);

			if (got <= 0) {
				/* the file shrank under us: the rest is zeros, so the archive stays whole */
				memset(piece, 0, (size_t)want);
				got = (ssize_t)want;
			}
			if (!out(piece, (uint64_t)got)) {
				close(in_fd);
				return 0;
			}
			left -= (uint64_t)got;
		}
		close(in_fd);
	}
	if (!out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/* The directory's own times once more, after its contents: making them moved them. */
static int
pack_dirtime(const char *relative, const struct stat *st)
{
	struct entry e;

	memset(&e, 0, sizeof(e));
	e.kind = ENT_DIRTIME;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/*
 * The names of one directory, gathered and sorted. The archive is meant to come out the same
 * bytes for the same tree: the order getdirentries gives shifts as a directory is written to, and
 * then every byte after it in the archive moves, which is the difference between a save that
 * writes nothing into the flash and one that writes all of itself. Both arenas are used as stacks
 * -- a level marks them on the way in and gives them back on the way out.
 */
#define NAME_ARENA      (2u << 20)
#define NAME_MAX_NAMES  65536

static char name_arena[NAME_ARENA];
static uint32_t name_at[NAME_MAX_NAMES];
static uint64_t arena_used, names_used, unsorted_dirs;

static void
sort_names(uint32_t *at, uint64_t n)
{
	/* insertion sort: a directory holds tens of names, and this keeps the program small */
	for (uint64_t i = 1; i < n; i++) {
		uint32_t v = at[i];
		uint64_t j = i;

		while (j > 0 && strcmp(name_arena + at[j - 1], name_arena + v) > 0) {
			at[j] = at[j - 1];
			j--;
		}
		at[j] = v;
	}
}

/* Every name in the open directory into the arena, sorted. Returns the count, or -1 if it did not fit. */
static long
gather_names(int dir_fd, char *buf, size_t bufsize, uint64_t mark_a, uint64_t mark_n)
{
	off_t position = 0;
	ssize_t n;

	while ((n = getdirentries(dir_fd, buf, bufsize, &position)) > 0) {
		for (ssize_t i = 0; i < n;) {
			struct dirent *d = (struct dirent *)(buf + i);
			const char *name = d->d_name;
			size_t len = strlen(name);

			i += d->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (arena_used + len + 1 > NAME_ARENA || names_used >= NAME_MAX_NAMES) {
				arena_used = mark_a;
				names_used = mark_n;
				return -1;
			}
			name_at[names_used++] = (uint32_t)arena_used;
			strcpy(name_arena + arena_used, name);
			arena_used += len + 1;
		}
	}
	sort_names(name_at + mark_n, names_used - mark_n);
	return (long)(names_used - mark_n);
}

/* relative: the path under ROOT, "" for ROOT itself. path holds the whole of it. */
static int
pack_tree(const char *relative)
{
	static char dirbuf[MAX_DEPTH][4096];    /* one read buffer a level */
	static int depth;
	struct stat st;
	size_t base = strlen(path), rbase = strlen(relative);
	int dir_fd, ok = 1;

	if (lstat(path, &st) != 0) {
		return 1;
	}
	if (relative[0] != 0 && !pack_one(relative, &st)) {
		return 0;
	}
	if ((st.st_mode & S_IFMT) != S_IFDIR || (relative[0] != 0 && is_skipped(relative))) {
		return 1;
	}
	if (depth >= MAX_DEPTH) {
		printf("var_store: %s: deeper than this program walks\n", path);
		return 1;
	}
	if ((dir_fd = open(path, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 1;
	}
	depth++;
	{
		uint64_t mark_a = arena_used, mark_n = names_used;
		long count = gather_names(dir_fd, dirbuf[depth - 1], sizeof(dirbuf[0]), mark_a, mark_n);

		if (count < 0) {
			/* more names than the arena holds: this one goes in whatever order it is given */
			unsorted_dirs++;
		}
		for (long k = 0; ok && k < count; k++) {
			const char *name = name_arena + name_at[mark_n + k];
			char child[MAX_PATH];           /* the caller's while this one recurses: not static */

			if (base + 1 + strlen(name) >= MAX_PATH || rbase + 1 + strlen(name) >= MAX_PATH) {
				printf("var_store: %s/%s: the path is too long\n", path, name);
				continue;
			}
			path[base] = '/';
			strcpy(path + base + 1, name);
			if (rbase > 0) {
				memcpy(child, relative, rbase);
				child[rbase] = '/';
				strcpy(child + rbase + 1, name);
			} else {
				strcpy(child, name);
			}
			ok = pack_tree(child);
			path[base] = 0;
		}
		arena_used = mark_a;
		names_used = mark_n;
	}
	depth--;
	close(dir_fd);
	if (ok && relative[0] != 0 && lstat(path, &st) == 0) {
		ok = pack_dirtime(relative, &st);
	}
	return ok;
}

static int
save(void)
{
	struct head old, h;
	int slot, best;
	static uint8_t block[BLOCK];

	memset(&old, 0, sizeof(old));
	best = newest(&old);
	slot = best < 0 ? 0 : (best + 1) % SLOTS;
	printf("var_store: saving into slot %d (slot %d holds generation %llu)\n", slot, best,
	    best < 0 ? 0ULL : (unsigned long long)old.generation);

	/* the slot's header first, so that a save cut short cannot leave an old header over a new archive */
	memset(block, 0, sizeof(block));
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	stream_start(slot, 0);
	entries = 0;
	last_saved_at = best < 0 ? 0 : old.seconds;
	changed_entries = changed_bytes = 0;
	skipped_blocks = written_blocks = 0;
	arena_used = names_used = unsorted_dirs = 0;
	/* a watcher saves more than once: nothing of the save before is carried into this one */
	memset(links, 0, LINKS * sizeof(*links));
	skipped_kinds = skipped_big = 0;
	strcpy(path, ROOT);
	if (!pack_tree("") || !out_flush(1)) {
		return 0;
	}
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = (best < 0 ? 0 : old.generation) + 1;
	h.bytes = stream_bytes;
	h.entries = entries;
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		h.seconds = (uint64_t)tv.tv_sec;
	}
	h.crc = stream_crc ^ 0xffffffffu;
	h.head_crc = head_checksum(&h);
	if (h.bytes > (uint64_t)SLOT_BYTES - BLOCK) {
		printf("var_store: the archive is %llu bytes, more than a slot holds (%llu): nothing saved\n",
		    (unsigned long long)h.bytes, (unsigned long long)(SLOT_BYTES - BLOCK));
		return 0;
	}
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	printf("var_store: slot %d: generation %llu, %llu entries, %llu bytes, crc %08x%s%s\n", slot,
	    (unsigned long long)h.generation, (unsigned long long)h.entries, (unsigned long long)h.bytes,
	    h.crc, skipped_kinds ? " (sockets and the like left out)" : "",
	    skipped_big ? " (something too big left out)" : "");
	if (last_saved_at != 0) {
		printf("var_store: %llu entries and %llu bytes of it were written since the last save "
		    "(%llu seconds ago)\n", (unsigned long long)changed_entries,
		    (unsigned long long)changed_bytes, (unsigned long long)(h.seconds - last_saved_at));
	}
	printf("var_store: %llu blocks into the flash, %llu left alone (they held this already)%s\n",
	    (unsigned long long)written_blocks, (unsigned long long)skipped_blocks,
	    unsorted_dirs ? " (a directory was too big to put in order)" : "");
	return 1;
}

/* ---- unpacking ---- */

static uint64_t made_dirs, made_files, made_links, made_bytes, short_files, lost_bytes;
/*
 * A belt, after the braces: the memory filesystem no longer answers ENOSPC for want of a page
 * (bsd/miscfs/ramfs, ramfs_grow_slots now takes an array found without the mount lock), so a short
 * write here means the volume really is full. Waiting a little and trying again still earns its
 * place -- the pageout path cannot wait for an array either, and it may be what answered -- but
 * the whole restore spends at most RETRY_MS on it, so a volume that is truly full does not turn
 * the boot into a crawl.
 */
#define RETRY_MS        2000
static uint64_t retry_ms_left = RETRY_MS;

static int
whole_path(const char *relative, uint32_t len)
{
	if (len == 0 || len >= MAX_PATH - sizeof(ROOT) - 1 || relative[0] == '/') {
		return 0;
	}
	for (uint32_t i = 0; i + 1 < len; i++) {
		if (relative[i] == '.' && relative[i + 1] == '.' &&
		    (i == 0 || relative[i - 1] == '/') && (i + 2 == len || relative[i + 2] == '/')) {
			return 0;       /* nothing of this archive is unpacked outside ROOT */
		}
	}
	strcpy(path, ROOT);
	path[sizeof(ROOT) - 1] = '/';
	memcpy(path + sizeof(ROOT), relative, len);
	path[sizeof(ROOT) + len] = 0;
	return 1;
}

static void
set_times(const struct entry *e)
{
	struct timeval times[2];

	times[0].tv_sec = (long)e->mtime_sec;
	times[0].tv_usec = (int)(e->mtime_nsec / 1000);
	times[1] = times[0];
	utimes(path, times);
}

/*
 * One entry out of the archive and into /private/var. make: with 0, the entry is only read past
 * (the first pass, which proves the archive's checksum before anything is made).
 */
static int
unpack_one(int make)
{
	struct entry e;
	static char relative[MAX_PATH];
	static uint8_t value[MAX_XATTR];
	static char name[1024];
	int out_fd = -1, ok = 1;

	if (!in(&e, sizeof(e))) {
		return 0;
	}
	if (e.pathlen >= MAX_PATH || e.nxattr > 4096) {
		printf("var_store: an entry this program cannot read (kind %u, path %u bytes)\n", e.kind,
		    e.pathlen);
		return 0;
	}
	if (!in(relative, e.pathlen)) {
		return 0;
	}
	relative[e.pathlen] = 0;
	if (make && !whole_path(relative, e.pathlen)) {
		make = 0;               /* read past it: an archive is not to make names of its own choosing */
	}
	if (make) {
		switch (e.kind) {
		case ENT_DIR:
			if (mkdir(path, (int)e.mode) == 0) {
				made_dirs++;
			}
			break;
		case ENT_FILE:
			out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)e.mode);
			if (out_fd < 0) {
				printf("var_store: %s cannot be made: %s\n", path, strerror(errno));
			} else {
				made_files++;
			}
			break;
		default:
			break;
		}
	}
	/* the attributes, before the contents, as they were packed */
	for (uint32_t i = 0; i < e.nxattr; i++) {
		uint32_t namelen, vlen;

		if (!in(&namelen, sizeof(namelen)) || !in(&vlen, sizeof(vlen))) {
			ok = 0;
			break;
		}
		if (namelen >= sizeof(name) || vlen > sizeof(value)) {
			ok = in_skip(namelen) && in_skip(vlen);
			skipped_big++;
			continue;
		}
		if (!in(name, namelen) || !in(value, vlen)) {
			ok = 0;
			break;
		}
		name[namelen] = 0;
		if (make && e.kind != ENT_HARDLINK) {
			setxattr(path, name, value, vlen, 0, XATTR_NOFOLLOW);
		}
	}
	if (!ok) {
		if (out_fd >= 0) {
			close(out_fd);
		}
		return 0;
	}
	/* the contents */
	if (e.kind == ENT_FILE) {
		static uint8_t piece[256 * 1024];
		uint64_t left = e.size, in_file = 0;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left, done = 0;

			if (!in(piece, want)) {
				ok = 0;
				break;
			}
			/*
			 * A whole data volume goes in at once, faster than the memory filesystem can find
			 * pages for it, and it then says ENOSPC for want of one page rather than for want
			 * of room (bsd/miscfs/ramfs, ramfs_grow_slots and the page limit). So a short write
			 * is waited out and tried again, and only a file that will not go in at all after a
			 * second of trying is given up on -- with its name, so it is known which.
			 */
			while (out_fd >= 0 && done < want) {
				ssize_t wrote = write(out_fd, piece + done, (size_t)(want - done));

				if (wrote > 0) {
					done += (uint64_t)wrote;
					continue;
				}
				if (retry_ms_left < 5) {
					printf("var_store: %s: %llu of %llu bytes, no more would go in: %s\n",
					    path, (unsigned long long)(in_file + done),
					    (unsigned long long)e.size, strerror(errno));
					short_files++;
					lost_bytes += e.size - in_file - done;
					close(out_fd);
					out_fd = -1;
					break;
				}
				usleep(5000);
				retry_ms_left -= 5;
			}
			in_file += done;
			left -= want;
		}
		if (make) {
			made_bytes += in_file;
		}
	} else if (e.kind == ENT_SYMLINK || e.kind == ENT_HARDLINK) {
		static char target[MAX_PATH];

		if (e.size >= MAX_PATH) {
			ok = in_skip(e.size);
		} else if (!in(target, e.size)) {
			ok = 0;
		} else {
			target[e.size] = 0;
			if (make && e.kind == ENT_SYMLINK) {
				/* a name the skeleton made too comes out as the store has it */
				unlink(path);
				if (symlink(target, path) == 0) {
					made_links++;
				}
			} else if (make) {
				static char from[MAX_PATH];
				char keep[MAX_PATH];

				strcpy(keep, path);
				if (whole_path(target, (uint32_t)e.size)) {
					strcpy(from, path);
					strcpy(path, keep);
					unlink(path);
					if (link(from, path) == 0) {
						made_links++;
					}
				} else {
					strcpy(path, keep);
				}
			}
		}
	}
	if (out_fd >= 0) {
		close(out_fd);
	}
	if (!ok || !in_pad()) {
		return 0;
	}
	if (!make) {
		return 1;
	}
	/*
	 * Owner, then permissions, then the times (writing moved them), and the flags last of all:
	 * one of them may be what keeps the file from being changed again.
	 */
	switch (e.kind) {
	case ENT_DIR:
		/* its flags wait for ENT_DIRTIME: one of them may be what stops anything being made in it */
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		break;
	case ENT_DIRTIME:
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	case ENT_SYMLINK:
		lchown(path, (int)e.uid, (int)e.gid);
		break;
	case ENT_HARDLINK:
		break;                  /* the same inode: it has them already */
	default:
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	}
	return 1;
}

static int
pass(const struct head *h, int make)
{
	stream_start((int)h->slot, h->bytes);
	for (uint64_t i = 0; i < h->entries; i++) {
		if (!unpack_one(make)) {
			return 0;
		}
	}
	if (stream_left != 0) {
		printf("var_store: %llu bytes left over after %llu entries\n",
		    (unsigned long long)stream_left, (unsigned long long)h->entries);
		return 0;
	}
	if ((stream_crc ^ 0xffffffffu) != h->crc) {
		printf("var_store: slot %u: the contents are not the %08x the header says (%08x)\n", h->slot,
		    h->crc, stream_crc ^ 0xffffffffu);
		return 0;
	}
	return 1;
}

static int
load(void)
{
	struct head h;
	int best;

	memset(&h, 0, sizeof(h));
	best = newest(&h);
	if (best < 0) {
		printf("var_store: no store yet: /private/var stays as init made it\n");
		return 1;
	}
	printf("var_store: slot %d: generation %llu of %llu seconds, %llu entries, %llu bytes\n", best,
	    (unsigned long long)h.generation, (unsigned long long)h.seconds,
	    (unsigned long long)h.entries, (unsigned long long)h.bytes);
	/* read once to prove the checksum, and only then make anything */
	if (!pass(&h, 0)) {
		struct head other;
		int left = -1;

		/* the newest is torn: the one before it, which a save never touches, may still be whole */
		for (int slot = 0; slot < SLOTS; slot++) {
			if (slot != best && read_head(slot, &other) && pass(&other, 0)) {
				left = slot;
				break;
			}
		}
		if (left < 0) {
			printf("var_store: nothing good in the store: /private/var stays as init made it\n");
			return 1;
		}
		h = other;
		printf("var_store: slot %d is good after all: generation %llu\n", left,
		    (unsigned long long)h.generation);
	}
	if (!pass(&h, 1)) {
		printf("var_store: the archive read differently the second time: what was made stays\n");
		return 0;
	}
	printf("var_store: %llu directories, %llu files (%llu bytes), %llu links back in /private/var\n",
	    (unsigned long long)made_dirs, (unsigned long long)made_files, (unsigned long long)made_bytes,
	    (unsigned long long)made_links);
	if (short_files != 0) {
		printf("var_store: %llu files did not fit: %llu bytes of them are not there\n",
		    (unsigned long long)short_files, (unsigned long long)lost_bytes);
	}
	return 1;
}

static void
show(void)
{
	for (int slot = 0; slot < SLOTS; slot++) {
		static uint8_t block[BLOCK];
		struct head *h = (struct head *)block;

		if (!device_read(slot_at(slot), block, BLOCK)) {
			continue;
		}
		if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0) {
			printf("var_store: slot %d: empty\n", slot);
			continue;
		}
		printf("var_store: slot %d: version %u, generation %llu, %llu entries, %llu bytes, crc %08x, "
		    "written at %llu, header %s\n", slot, h->version, (unsigned long long)h->generation,
		    (unsigned long long)h->entries, (unsigned long long)h->bytes, h->crc,
		    (unsigned long long)h->seconds, h->head_crc == head_checksum(h) ? "right" : "WRONG");
	}
}

static int
erase(void)
{
	static uint8_t block[BLOCK];

	memset(block, 0, sizeof(block));
	for (int slot = 0; slot < SLOTS; slot++) {
		if (!device_write(slot_at(slot), block, BLOCK)) {
			return 0;
		}
	}
	printf("var_store: both slots forgotten\n");
	return 1;
}

/*
 * Stay and save by itself. The kernel is told there is a saver, and then this waits: for the
 * kernel to ask (the run is ending, and it waits for the answer before it goes), and, if seconds
 * is given, for that long to pass between saves of its own accord. A save that fails is said so
 * and the waiting goes on -- the one at the end of the run still gets its chance.
 */
static int
save_once(void)
{
	int ok;

	/*
	 * The device is opened for the save and closed again, not held: while a write is in flight
	 * this kernel's own transfer list is out of the controller, and it goes back in when the
	 * device is closed (bsd/dev/redwood_ufs.c, ufs_cclose). A watcher that kept the device open
	 * would leave iOS reading the flash through the loader for the rest of the run.
	 */
	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = claim() && save();
	release();
	close(fd);
	fd = -1;
	return ok;
}

static int
watch(int seconds)
{
	uint64_t last = 0;
	int asked = 0;

	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s cannot be written (%s): nothing would be saved\n", DEVICE,
		    strerror(errno));
		return 0;
	}
	close(fd);
	fd = -1;
	if (sysctl_set_int("kern.redwood.var_save", VAR_SAVE_IDLE) != 0) {
		printf("var_store: this kernel does not keep the data volume (%s)\n", strerror(errno));
		return 0;
	}
	printf("var_store: watching: the volume is saved when the run ends%s\n",
	    seconds > 0 ? ", and in between" : "");
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		last = (uint64_t)tv.tv_sec;
	}
	for (;;) {
		struct timeval tv = { 0, 0 };
		int state = VAR_SAVE_NONE;

		if (sysctl_int("kern.redwood.var_save", &state) == 0 && state == VAR_SAVE_WANTED) {
			asked = 1;
		}
		gettimeofday(&tv, NULL);
		if (asked || (seconds > 0 && (uint64_t)tv.tv_sec - last >= (uint64_t)seconds)) {
			int ok = save_once();

			if (!ok) {
				printf("var_store: this save came to nothing; the store keeps what it had\n");
			}
			last = (uint64_t)tv.tv_sec;
			if (asked) {
				/* answered either way: the kernel is waiting on this to go back to the loader */
				(void)sysctl_set_int("kern.redwood.var_save", VAR_SAVE_DONE);
				return ok;
			}
		}
		usleep(100000);
	}
}

/* A line saying when, for a run to leave behind and the next one to find. */
static int
stamp(const char *where)
{
	struct timeval tv = { 0, 0 };
	char line[256];
	int out_fd = open(where, O_WRONLY | O_CREAT | O_APPEND, 0644);
	int n;

	if (out_fd < 0) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		return 0;
	}
	gettimeofday(&tv, NULL);
	n = 0;
	{
		static const char head[] = "written by run of ";
		unsigned long long seconds = (unsigned long long)tv.tv_sec;
		char digits[24];
		int k = 0;

		memcpy(line, head, sizeof(head) - 1);
		n = (int)sizeof(head) - 1;
		do {
			digits[k++] = (char)('0' + seconds % 10);
			seconds /= 10;
		} while (seconds > 0);
		while (k-- > 0) {
			line[n++] = digits[k];
		}
		line[n++] = '\n';
	}
	if (write(out_fd, line, (size_t)n) != n) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		close(out_fd);
		return 0;
	}
	close(out_fd);
	printf("var_store: %s: %d bytes added\n", where, n);
	return 1;
}

int
main(int argc, char **argv, char **envp)
{
	const char *what = argc > 1 ? argv[1] : "show";
	int ok;

	(void)envp;
	crc_init();
	if (strcmp(what, "stamp") == 0) {
		return stamp(argc > 2 ? argv[2] : "/private/var/redwood-stamp") ? 0 : 1;
	}
	buffer = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	links = mmap(NULL, LINKS * sizeof(*links), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (buffer == MAP_FAILED || links == MAP_FAILED) {
		printf("var_store: no memory for the buffers\n");
		return 1;
	}
	/* for comparing with what the slot holds before writing over it; doing without is only slower */
	already = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (already == MAP_FAILED) {
		already = NULL;
	}
	if (strcmp(what, "watch") == 0) {
		if (!boot_arg_is("var_store", "1")) {
			printf("var_store: not asked to keep the data volume (var_store=1): nothing to watch\n");
			return 0;
		}
		return watch(argc > 2 ? (int)parse_number(argv[2]) : 0) ? 0 : 1;
	}
	fd = open(DEVICE, strcmp(what, "load") == 0 || strcmp(what, "show") == 0 ? O_RDONLY : O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s (the kernel takes writes with ufs_write=1)\n", DEVICE,
		    strerror(errno));
		return 1;
	}
	if (strcmp(what, "save") == 0) {
		ok = claim() && save();
		release();
	} else if (strcmp(what, "load") == 0) {
		ok = load();
	} else if (strcmp(what, "erase") == 0) {
		ok = claim() && erase();        /* a write like any other: the claim first */
		release();
	} else {
		show();
		ok = 1;
	}
	close(fd);
	return ok ? 0 : 1;
}
