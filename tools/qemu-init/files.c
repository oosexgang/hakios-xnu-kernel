/* tools/qemu-init: files and file systems: ls, cat, mount, mkdir, write, rm, stat, ln, mv, chmod, mmap */
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
		list("/");
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
			say(argv[i]);
			say(": cannot be read\n");
			continue;
		}
		for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0;) {
			syscall6(SYS_write, console, (long)buffer, got, 0, 0, 0);
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
		say("mount: not mounted, error ");
		say_number((uint64_t)-error);
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

static void
say_failed(const char *what, const char *path, long error)
{
	say(what);
	say(": ");
	say(path);
	say(": error ");
	say_number((uint64_t)-error);
	say("\n");
}

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
			say(argv[i]);
			say(": cannot be read\n");
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
		say("mmap: not mapped, error ");
		say_number((uint64_t)-mapped);
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
		say("memory: not given, error ");
		say_number((uint64_t)-mapped);
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
