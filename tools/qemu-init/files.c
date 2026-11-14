/* tools/qemu-init: files and file systems: ls, cat, mount, mkdir, write, rm, stat, ln, mv, chmod, xattr, files, fill, mmap, mmap-write,
 * descriptors, df */
#include "init.h"

#define SYS_lstat64             340
#define SYS_getdirentries64     344
#define O_RDONLY                0
#define O_DIRECTORY             0x100000

struct stat64_head {                    /* the start of Darwin's struct stat64, up to st_size */
	int32_t         dev;
	uint16_t        mode, nlink;
	uint64_t        ino;
	uint32_t        uid, gid, rdev, pad;
	int64_t         times[8];       /* access, modification, change, birth: seconds and nanoseconds */
	int64_t         size;
	char            rest[64];
};

/* the kind and permissions as ls -l writes them */
static void
say_mode(uint16_t mode)
{
	static const char kinds[16] = { '?', 'p', 'c', '?', 'd', '?', 'b', '?', '-', '?', 'l', '?', 's', '?', 'w', '?' };
	char text[11];

	text[0] = kinds[(mode >> 12) & 0xf];
	for (int i = 0; i < 9; i++) {
		text[1 + i] = (mode >> (8 - i)) & 1 ? "rwxrwxrwx"[i] : '-';
	}
	text[10] = '\0';
	say(text);
}

/* "WHAT: PATH: error N (ENAME)" */
static void
say_failed(const char *what, const char *path, long error)
{
	say(what);
	say(": ");
	say(path);
	say(": ");
	say_error(error);
	say("\n");
}

static void
list(const char *path)
{
	char buffer[4096], full[512];
	int64_t base = 0;
	long fd = syscall6(SYS_open, (long)path, O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);

	if (fd < 0) {
		say(path);
		say(": cannot be listed\n");
		return;
	}
	for (;;) {
		long got = syscall6(SYS_getdirentries64, fd, (long)buffer, sizeof(buffer), (long)&base, 0, 0);
		if (got <= 0) {
			break;
		}
		for (long at = 0; at < got;) {
			/* struct direntry: d_ino, d_seekoff, d_reclen, d_namlen, d_type, d_name */
			uint16_t reclen = *(uint16_t *)(buffer + at + 16);
			const char *name = buffer + at + 21;
			struct stat64_head st;
			size_t n = 0;

			for (const char *c = path; *c != '\0' && n < sizeof(full) - 2; c++) {
				full[n++] = *c;
			}
			if (n > 0 && full[n - 1] != '/') {
				full[n++] = '/';
			}
			for (const char *c = name; *c != '\0' && n < sizeof(full) - 1; c++) {
				full[n++] = *c;
			}
			full[n] = '\0';
			if (syscall6(SYS_lstat64, (long)full, (long)&st, 0, 0, 0, 0) == 0) {
				say_mode(st.mode);
				say_padded((uint64_t)st.size, 10);
				say("  ");
			} else {
				say("?????????? ?         ");
			}
			say(name);
			say("\n");
			at += reclen;
		}
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}

void
command_ls(int argc, char **argv)
{
	if (argc < 2) {
		list(".");
	}
	for (int i = 1; i < argc; i++) {
		if (argc > 2) {
			say(argv[i]);
			say(":\n");
		}
		list(argv[i]);
	}
}

void
command_cat(int argc, char **argv)
{
	char buffer[1024];

	for (int i = 1; i < argc; i++) {
		long fd = syscall6(SYS_open, (long)argv[i], O_RDONLY, 0, 0, 0, 0);
		if (fd < 0) {
			say_failed("cat", argv[i], fd);
			continue;
		}
		char last = '\n';
		for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0;) {
			syscall6(SYS_write, console, (long)buffer, got, 0, 0, 0);
			last = buffer[got - 1];
		}
		if (last != '\n') {
			say("\n");             /* (a file that does not end a line: the prompt still starts one) */
		}
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
}

#define SYS_mount               167
#define MNT_RDONLY              0x1

/* TYPE DEVICE DIRECTORY, read only: what the file system takes as its data is its own (APFS: the device) */
void
command_mount(int argc, char **argv)
{
	if (argc != 4) {
		say("mount TYPE DEVICE DIRECTORY: mount apfs /dev/rvblk0 /mnt (read only), mount ramfs ramfs /tmp\n");
		return;
	}
	/* (a disk read only; ramfs, the kernel's file system in memory, to be written) */
	long error = syscall6(SYS_mount, (long)argv[1], (long)argv[3], same(argv[1], "ramfs") ? 0 : MNT_RDONLY, (long)argv[2], 0, 0);
	if (error < 0) {
		say("mount: not mounted, ");
		say_error(error);
		say("\n");
		return;
	}
	say(argv[2]);
	say(" on ");
	say(argv[3]);
	say("\n");
	list(argv[3]);
}

#define SYS_unlink              10
#define SYS_mkdir               136
#define SYS_rmdir               137
#define O_CREAT                 0x200
#define O_TRUNC                 0x400

/* DIR...: new directories */
void
command_mkdir(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		long error = syscall6(SYS_mkdir, (long)argv[i], 0755, 0, 0, 0, 0);
		if (error != 0) {
			say_failed("mkdir", argv[i], error);
		}
	}
}

/* FILE WORDS...: the words, and a new line, as all of FILE */
void
command_write(int argc, char **argv)
{
	if (argc < 2) {
		say("write FILE WORDS...\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
	if (fd < 0) {
		say_failed("write", argv[1], fd);
		return;
	}
	long written = 0;
	for (int i = 2; i < argc; i++) {
		long n = (long)length(argv[i]);

		written += syscall6(SYS_write, fd, (long)argv[i], n, 0, 0, 0) == n ? n : 0;
		written += syscall6(SYS_write, fd, (long)(i + 1 < argc ? " " : "\n"), 1, 0, 0, 0) == 1;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("write: ");
	say_number((uint64_t)written);
	say(" bytes to ");
	say(argv[1]);
	say("\n");
}

/* PATH...: files, or empty directories, gone */
void
command_rm(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		long error = syscall6(SYS_unlink, (long)argv[i], 0, 0, 0, 0, 0);
		if (error != 0) {
			error = syscall6(SYS_rmdir, (long)argv[i], 0, 0, 0, 0, 0);
		}
		if (error != 0) {
			say_failed("rm", argv[i], error);
		}
	}
}

#define SYS_link                9
#define SYS_chmod               15
#define SYS_symlink             57
#define SYS_readlink            58
#define SYS_rename              128

/* PATH...: what lstat says of each: its kind and permissions, its links, its inode and its size, and where a
 * symbolic link points */
void
command_stat(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		struct stat64_head st;
		char target[256];
		long error = syscall6(SYS_lstat64, (long)argv[i], (long)&st, 0, 0, 0, 0), n;

		if (error != 0) {
			say_failed("stat", argv[i], error);
			continue;
		}
		say(argv[i]);
		say(": ");
		say_mode(st.mode);
		say(" ");
		say_number(st.nlink);
		say(st.nlink == 1 ? " link, inode " : " links, inode ");
		say_number(st.ino);
		say(", ");
		say_number((uint64_t)st.size);
		say(" bytes");
		if ((st.mode & 0170000) == 0120000 &&
		    (n = syscall6(SYS_readlink, (long)argv[i], (long)target, sizeof(target) - 1, 0, 0, 0)) >= 0) {
			target[n] = '\0';
			say(" -> ");
			say(target);
		}
		say("\n");
	}
}

/* [-s] TARGET NAME: another name for a file (a hard link), or with -s a symbolic link to TARGET */
void
command_ln(int argc, char **argv)
{
	int symbolic = argc == 4 && same(argv[1], "-s");
	long error;

	if (argc != 3 && !symbolic) {
		say("ln [-s] TARGET NAME\n");
		return;
	}
	error = syscall6(symbolic ? SYS_symlink : SYS_link, (long)argv[argc - 2], (long)argv[argc - 1], 0, 0, 0, 0);
	if (error != 0) {
		say_failed("ln", argv[argc - 1], error);
	}
}

/* OLD NEW: a file or directory renamed (in place of NEW, if that is there) */
void
command_mv(int argc, char **argv)
{
	long error;

	if (argc != 3) {
		say("mv OLD NEW\n");
		return;
	}
	error = syscall6(SYS_rename, (long)argv[1], (long)argv[2], 0, 0, 0, 0);
	if (error != 0) {
		say_failed("mv", argv[1], error);
	}
}

/* MODE PATH...: the permissions, MODE in octal */
void
command_chmod(int argc, char **argv)
{
	unsigned mode = 0;
	const char *c;

	for (c = argc > 1 ? argv[1] : ""; *c >= '0' && *c <= '7'; c++) {
		mode = mode * 8 + (unsigned)(*c - '0');
	}
	if (argc < 3 || *c != '\0' || c == argv[1] || mode > 07777) {
		say("chmod MODE PATH... (MODE in octal)\n");
		return;
	}
	for (int i = 2; i < argc; i++) {
		long error = syscall6(SYS_chmod, (long)argv[i], mode, 0, 0, 0, 0);
		if (error != 0) {
			say_failed("chmod", argv[i], error);
		}
	}
}

#define SYS_getxattr            234
#define SYS_setxattr            236
#define SYS_removexattr         238
#define SYS_listxattr           240
#define XATTR_NOFOLLOW          1

/* FILE: its extended attributes and their values; FILE NAME VALUE: one set; -d FILE NAME: one taken away */
void
command_xattr(int argc, char **argv)
{
	char names[1024], value[256];
	long error, n;

	if (argc == 4 && same(argv[1], "-d")) {
		error = syscall6(SYS_removexattr, (long)argv[2], (long)argv[3], XATTR_NOFOLLOW, 0, 0, 0);
		if (error != 0) {
			say_failed("xattr", argv[3], error);
		}
		return;
	}
	if (argc == 4) {
		error = syscall6(SYS_setxattr, (long)argv[1], (long)argv[2], (long)argv[3], length(argv[3]), 0, XATTR_NOFOLLOW);
		if (error != 0) {
			say_failed("xattr", argv[2], error);
		}
		return;
	}
	if (argc != 2) {
		say("xattr FILE | xattr FILE NAME VALUE | xattr -d FILE NAME\n");
		return;
	}
	n = syscall6(SYS_listxattr, (long)argv[1], (long)names, sizeof(names), XATTR_NOFOLLOW, 0, 0);
	if (n < 0) {
		say_failed("xattr", argv[1], n);
		return;
	}
	if (n == 0) {
		say(argv[1]);
		say(": no extended attributes\n");
	}
	for (long at = 0; at < n; at += length(names + at) + 1) {
		long got = syscall6(SYS_getxattr, (long)argv[1], (long)(names + at), (long)value, sizeof(value) - 1, 0, XATTR_NOFOLLOW);

		say(names + at);
		if (got < 0) {
			say(": cannot be read, ");
			say_error(got);
			say("\n");
			continue;
		}
		value[got] = '\0';
		say(" = ");
		say(value);
		say("\n");
	}
}

#define SYS_getrlimit           194
#define SYS_setrlimit           195
#define RLIMIT_NOFILE           8

/* [LIMIT]: the descriptors a process may have, lowered to LIMIT (20 without one), and files opened until the
 * kernel says no; all closed again and the limit put back */
void
command_files(int argc, char **argv)
{
	struct { uint64_t current, most; } was, lowered;
	unsigned limit = argc > 1 ? parse_number(argv[1]) : 20;
	long fds[256], fd = 0;
	unsigned opened = 0;

	if (limit < 4 || limit > 256) {
		say("files [LIMIT]: LIMIT from 4 to 256\n");
		return;
	}
	if (syscall6(SYS_getrlimit, RLIMIT_NOFILE, (long)&was, 0, 0, 0, 0) != 0) {
		say("files: getrlimit failed\n");
		return;
	}
	lowered = was;
	lowered.current = limit;
	if (syscall6(SYS_setrlimit, RLIMIT_NOFILE, (long)&lowered, 0, 0, 0, 0) != 0) {
		say("files: setrlimit failed\n");
		return;
	}
	while (opened < 256 && (fd = syscall6(SYS_open, (long)"/etc/motd", O_RDONLY, 0, 0, 0, 0)) >= 0) {
		fds[opened++] = fd;
	}
	say("files: ");
	say_number(opened);
	say(" opened under a limit of ");
	say_number(limit);
	say(" descriptors, the last ");
	say_number(opened ? (uint64_t)fds[opened - 1] : 0);
	say("; then ");
	say_error(fd);
	say("\n");
	while (opened > 0) {
		syscall6(SYS_close, fds[--opened], 0, 0, 0, 0, 0);
	}
	syscall6(SYS_setrlimit, RLIMIT_NOFILE, (long)&was, 0, 0, 0, 0);
}

#define SYS_munmap              73
#define SYS_mmap                197
#define SYS_fstat64             339
#define PROT_READ               1
#define MAP_PRIVATE             2

/* FNV-1a, over bytes, from where it got to */
static uint64_t
fnv(uint64_t hash, const uint8_t *bytes, long n)
{
	for (long i = 0; i < n; i++) {
		hash = (hash ^ bytes[i]) * 0x100000001b3ull;
	}
	return hash;
}

/* FILE...: each read to its end, timed, and its FNV-1a hash (a device as well as a file) */
void
command_hash(int argc, char **argv)
{
	static uint8_t buffer[65536];          /* (a multiple of any block: a raw device takes whole ones) */

	for (int i = 1; i < argc; i++) {
		long fd = syscall6(SYS_open, (long)argv[i], O_RDONLY, 0, 0, 0, 0);
		if (fd < 0) {
			say_failed("hash", argv[i], fd);
			continue;
		}
		uint64_t hash = 0xcbf29ce484222325ull, all = 0;
		int64_t start = now_us();
		for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += (uint64_t)got) {
			hash = fnv(hash, buffer, got);
		}
		int64_t took = now_us() - start;
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		char line[200], digits[17];
		for (int d = 0; d < 16; d++) {
			digits[d] = "0123456789abcdef"[(hash >> (60 - 4 * d)) & 15];
		}
		digits[16] = '\0';
		size_t at = append(line, append(line, 0, sizeof(line), argv[i]), sizeof(line), ": ");
		at = append_number(line, at, sizeof(line), all);
		at = append_number(line, append(line, at, sizeof(line), " bytes in "), sizeof(line), (uint64_t)took / 1000);
		at = append(line, append(line, at, sizeof(line), " ms, fnv1a "), sizeof(line), digits);
		append(line, at, sizeof(line), "\n");
		say(line);
	}
}

/* FILE: the file mapped (its pages come from its file system), checked against what read() gives */
void
command_mmap(int argc, char **argv)
{
	static uint8_t buffer[16384];
	struct stat64_head st;

	if (argc != 2) {
		say("mmap FILE\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size <= 0) {
		say("mmap: ");
		say(argv[1]);
		say(fd < 0 ? ": cannot be read\n" : ": nothing to map\n");
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	long mapped = syscall6(SYS_mmap, 0, st.size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped < 0) {
		say("mmap: not mapped, ");
		say_error(mapped);
		say("\n");
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	uint64_t by_pages = fnv(0xcbf29ce484222325ull, (const uint8_t *)mapped, st.size), by_reads = 0xcbf29ce484222325ull;
	long all = 0;
	for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += got) {
		by_reads = fnv(by_reads, buffer, got);
	}
	syscall6(SYS_munmap, mapped, st.size, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("mmap: ");
	say(argv[1]);
	say(", ");
	say_number((uint64_t)st.size);
	say(" bytes: ");
	say(all == st.size && by_pages == by_reads ? "its pages and its reads agree\n" : "its pages and its reads DIFFER\n");
}

/* ---- memory: pages of its own ---- */

#define PROT_WRITE              2
#define MAP_ANON                0x1000

/* the kernel's free memory, in megabytes (vm.page_free_count pages of hw.pagesize) */
static uint64_t
free_megabytes(void)
{
	uint64_t pages = 0, page_size = 0;

	sysctl_number("vm.page_free_count", &pages);
	sysctl_number("hw.pagesize", &page_size);
	return pages * page_size >> 20;
}

/* MB: that much memory of its own (anonymous, mmap), every page written and read back, then given back */
void
command_memory(int argc, char **argv)
{
	unsigned megabytes = argc > 1 ? parse_number(argv[1]) : 0;

	if (megabytes < 1 || megabytes > 1024) {
		say("memory MB: 1 to 1024\n");
		return;
	}
	uint64_t size = (uint64_t)megabytes << 20, before = free_megabytes();
	long mapped = syscall6(SYS_mmap, 0, (long)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapped < 0) {
		say("memory: not given, ");
		say_error(mapped);
		say("\n");
		return;
	}
	volatile uint64_t *words = (volatile uint64_t *)mapped;
	uint64_t count = size / sizeof(uint64_t), wrong = 0;
	int64_t start = now_us();
	for (uint64_t i = 0; i < count; i += 512) {             /* (a word every 4 KB: every page) */
		words[i] = i * 0x9e3779b97f4a7c15ull;
	}
	uint64_t during = free_megabytes();
	for (uint64_t i = 0; i < count; i += 512) {
		wrong += words[i] != i * 0x9e3779b97f4a7c15ull;
	}
	int64_t took = now_us() - start;
	syscall6(SYS_munmap, mapped, (long)size, 0, 0, 0, 0);
	say("memory: ");
	say_number(megabytes);
	say(" MB written and read back in ");
	say_number((uint64_t)took / 1000);
	say(wrong == 0 ? " ms, all as written" : " ms, SOME NOT AS WRITTEN");
	say("; free: ");
	say_number(before);
	say(" MB before, ");
	say_number(during);
	say(" MB with it, ");
	say_number(free_megabytes());
	say(" MB after\n");
}

#define SYS_msync               65
#define PROT_WRITE              2
#define MAP_SHARED              1
#define MS_SYNC                 0x10

/* FILE BYTES: a file of that many bytes made up as they go (byte i is i * 7 mod 251), written in pieces of 16 KB */
void
command_fill(int argc, char **argv)
{
	static uint8_t chunk[16384];
	uint64_t bytes = argc == 3 ? parse_number(argv[2]) : 0, done = 0;
	long fd;

	if (argc != 3 || bytes == 0) {
		say("fill FILE BYTES\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
	if (fd < 0) {
		say_failed("fill", argv[1], fd);
		return;
	}
	while (done < bytes) {
		uint64_t n = bytes - done < sizeof(chunk) ? bytes - done : sizeof(chunk);

		for (uint64_t i = 0; i < n; i++) {
			chunk[i] = (uint8_t)((done + i) * 7 % 251);
		}
		long wrote = syscall6(SYS_write, fd, (long)chunk, (long)n, 0, 0, 0);
		if (wrote != (long)n) {
			say_failed("fill", argv[1], wrote < 0 ? wrote : -28);
			break;
		}
		done += n;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("fill: ");
	say(argv[1]);
	say(", ");
	say_number(done);
	say(" bytes\n");
}

/*
 * FILE: the file mapped shared and written through the mapping -- every byte turned over (b ^ 0xff) -- then
 * msync'd and unmapped: what reading it then gives must be the turned bytes (the file system's page-out)
 */
void
command_mmap_write(int argc, char **argv)
{
	static uint8_t buffer[16384];
	struct stat64_head st;
	uint64_t before = 0xcbf29ce484222325ull, after = 0xcbf29ce484222325ull, read_back = 0xcbf29ce484222325ull;
	long fd, mapped, all = 0;

	if (argc != 2) {
		say("mmap-write FILE\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], 2 /* O_RDWR */, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size <= 0) {
		say_failed("mmap-write", argv[1], fd < 0 ? fd : -22);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	mapped = syscall6(SYS_mmap, 0, st.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped < 0) {
		say_failed("mmap-write", argv[1], mapped);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	for (int64_t i = 0; i < st.size; i++) {
		uint8_t *b = (uint8_t *)mapped + i;

		before = fnv(before, b, 1);
		*b ^= 0xff;
		after = fnv(after, b, 1);
	}
	syscall6(SYS_msync, mapped, st.size, MS_SYNC, 0, 0, 0);
	syscall6(SYS_munmap, mapped, st.size, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0);
	for (long got; fd >= 0 && (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += got) {
		read_back = fnv(read_back, buffer, got);
	}
	if (fd >= 0) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	say("mmap-write: ");
	say(argv[1]);
	say(", ");
	say_number((uint64_t)st.size);
	say(all == st.size && read_back == after && after != before ? " bytes turned through the mapping, and read so\n" :
	    " bytes: what is read is NOT what the mapping wrote\n");
}

#define SYS_dup                 41
#define SYS_fcntl               92
#define SYS_dup2                90
#define SYS_pread               153
#define SYS_pwrite              154
#define SYS_lseek               199
#define O_RDWR                  2
#define O_APPEND                8
#define O_NONBLOCK              4
#define F_SETFL                 4

/* the n bytes at `at' of what was read are those of `want' */
static int
read_is(long fd, const char *want, long n)
{
	char got[16];

	return n <= (long)sizeof(got) && syscall6(SYS_read, fd, (long)got, n, 0, 0, 0) == n && bytes_same(got, want, (size_t)n);
}

/*
 * FILE (made anew): what descriptors do, as BSD has them -- an offset moved by lseek, pread and pwrite that move
 * none, a dup's offset the same as its original's, dup2 onto a number of one's choosing, O_APPEND writing at the
 * end whatever the offset, and a non-blocking pipe with nothing in it saying EAGAIN
 */
void
command_descriptors(int argc, char **argv)
{
	char line[400], two[2];
	size_t at;
	long fd, copy, pipe_result;
	int ends[2];
	struct { const char *what; int ok; } said[7];
	int n = 0, all = 1;

	if (argc != 2) {
		say("descriptors FILE (made anew)\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
	if (fd < 0) {
		say_failed("descriptors", argv[1], fd);
		return;
	}
	syscall6(SYS_write, fd, (long)"0123456789", 10, 0, 0, 0);
	said[n].what = "lseek";
	said[n++].ok = syscall6(SYS_lseek, fd, 3, 0 /* SEEK_SET */, 0, 0, 0) == 3 && read_is(fd, "345", 3);
	said[n].what = "pread";
	said[n++].ok = syscall6(SYS_pread, fd, (long)two, 2, 7, 0, 0) == 2 && bytes_same(two, "78", 2) && read_is(fd, "6", 1);
	said[n].what = "pwrite";
	said[n++].ok = syscall6(SYS_pwrite, fd, (long)"ab", 2, 0, 0, 0) == 2 && read_is(fd, "7", 1);
	copy = syscall6(SYS_dup, fd, 0, 0, 0, 0, 0);
	said[n].what = "a dup's offset";
	said[n++].ok = copy >= 0 && read_is(copy, "8", 1) && read_is(fd, "9", 1);
	said[n].what = "dup2";
	said[n++].ok = syscall6(SYS_dup2, fd, 20, 0, 0, 0, 0) == 20 && syscall6(SYS_lseek, 20, 0, 0, 0, 0, 0) == 0 && read_is(fd, "ab", 2);
	syscall6(SYS_close, 20, 0, 0, 0, 0, 0);
	syscall6(SYS_close, copy, 0, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	fd = syscall6(SYS_open, (long)argv[1], O_RDWR | O_APPEND, 0, 0, 0, 0);
	said[n].what = "O_APPEND";
	said[n++].ok = fd >= 0 && syscall6(SYS_lseek, fd, 0, 0, 0, 0, 0) == 0 && syscall6(SYS_write, fd, (long)"!", 1, 0, 0, 0) == 1 &&
	    syscall6(SYS_pread, fd, (long)two, 1, 10, 0, 0) == 1 && two[0] == '!';
	if (fd >= 0) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	pipe_result = syscall_pipe(ends);
	said[n].what = "a non-blocking pipe's EAGAIN";
	said[n++].ok = pipe_result == 0 && syscall6(SYS_fcntl, ends[0], F_SETFL, O_NONBLOCK, 0, 0, 0) == 0 &&
	    syscall6(SYS_read, ends[0], (long)two, 1, 0, 0, 0) == -35;
	if (pipe_result == 0) {
		syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
		syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	}
	at = append(line, 0, sizeof(line), "descriptors:");
	for (int i = 0; i < n; i++) {
		all &= said[i].ok;
		at = append(line, at, sizeof(line), " ");
		at = append(line, at, sizeof(line), said[i].what);
		at = append(line, at, sizeof(line), said[i].ok ? "," : " WRONG,");
	}
	at = append(line, at - 1, sizeof(line), all ? ": as BSD has them\n" : ": NOT all as BSD has them\n");
	say(line);
}

#define SYS_getfsstat64         347
#define MNT_WAIT                1       /* (asked of each file system now, not as it was at its mount) */

struct statfs64 {                       /* Darwin's struct statfs (64-bit inodes) */
	uint32_t bsize;
	int32_t iosize;
	uint64_t blocks, bfree, bavail, files, ffree;
	int32_t fsid[2];
	uint32_t owner, type, flags, fssubtype;
	char fstypename[16], mntonname[1024], mntfromname[1024];
	uint32_t reserved[8];
};

/* the file systems mounted: where, of what kind, from what, and their size and room in KB (getfsstat64) */
void
command_df(int argc, char **argv)
{
	static struct statfs64 all[16];
	long n = syscall6(SYS_getfsstat64, (long)all, sizeof(all), MNT_WAIT, 0, 0, 0);

	(void)argc;
	(void)argv;
	if (n < 0) {
		say("df: ");
		say_error(n);
		say("\n");
		return;
	}
	for (long i = 0; i < n && i < 16; i++) {
		char line[256];
		size_t at = append(line, 0, sizeof(line), all[i].mntonname);

		at = append(line, at, sizeof(line), ": ");
		at = append(line, at, sizeof(line), all[i].fstypename);
		at = append(line, at, sizeof(line), " from ");
		at = append(line, at, sizeof(line), all[i].mntfromname);
		at = append(line, at, sizeof(line), ", ");
		at = append_number(line, at, sizeof(line), all[i].blocks * all[i].bsize / 1024);
		at = append(line, at, sizeof(line), " KB, ");
		at = append_number(line, at, sizeof(line), all[i].bavail * all[i].bsize / 1024);
		at = append(line, at, sizeof(line), " KB free");
		at = append(line, at, sizeof(line), all[i].flags & 1 ? ", read only\n" : "\n");
		say(line);
	}
}
