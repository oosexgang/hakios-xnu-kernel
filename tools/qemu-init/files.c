/* tools/qemu-init: files and file systems: ls, cat, mount, mkdir, write, rm, stat, ln, mv, chmod, chown, umask,
 * xattr, files, hash, fill, descriptors, df, truncate, touch, vectors, chflags, access, cp, fsize, opens, dir-walk */
#include "init.h"

#define SYS_lstat64             340
#define SYS_getdirentries64     344
#define O_RDONLY                0
#define O_DIRECTORY             0x100000

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
	/* set-user-ID, set-group-ID, sticky: in the place of their x, s (t) over an x, S (T) without one */
	if (mode & 04000) {
		text[3] = text[3] == 'x' ? 's' : 'S';
	}
	if (mode & 02000) {
		text[6] = text[6] == 'x' ? 's' : 'S';
	}
	if (mode & 01000) {
		text[9] = text[9] == 'x' ? 't' : 'T';
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
#define O_APPEND                8

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
	int adding = argc > 1 && same(argv[1], "-a");     /* -a: added to its end (O_APPEND), not written anew */

	argc -= adding;
	argv += adding;
	if (argc < 2) {
		say("write [-a] FILE WORDS...\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | (adding ? O_APPEND : O_TRUNC), 0644, 0, 0, 0);
	if (fd < 0) {
		say_failed("write", argv[1], fd);
		return;
	}
	long written = 0, error = 0, got;
	for (int i = 2; i < argc && error == 0; i++) {
		long n = (long)length(argv[i]);

		if ((got = syscall6(SYS_write, fd, (long)argv[i], n, 0, 0, 0)) < 0 ||
		    (got = syscall6(SYS_write, fd, (long)(i + 1 < argc ? " " : "\n"), 1, 0, 0, 0)) < 0) {
			error = got;            /* (said: a write the file system refused, ENOSPC as often as not) */
		} else {
			written += n + 1;
		}
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (error != 0) {
		say_failed("write", argv[1], error);
	}
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
		/* (a directory by rmdir, anything else by unlink: the error said is the one of the call that fits; a file
		 * that will not go is not said to be "not a directory") */
		struct stat64_head st;
		long error = syscall6(SYS_lstat64, (long)argv[i], (long)&st, 0, 0, 0, 0);

		if (error == 0) {
			error = syscall6((st.mode & 0170000) == 0040000 ? SYS_rmdir : SYS_unlink, (long)argv[i], 0, 0, 0, 0, 0);
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
		say(" bytes, owner ");
		say_number(st.uid);
		say(":");
		say_number(st.gid);
		uint32_t flags;

		copy_bytes(&flags, st.rest + 12, 4);    /* (st_blocks, st_blksize, then st_flags) */
		if (flags != 0) {
			say(", flags 0x");
			for (int shift = 28; shift >= 0; shift -= 4) {
				if ((flags >> shift) != 0) {
					char digit[2] = { "0123456789abcdef"[(flags >> shift) & 0xf], '\0' };

					say(digit);
				}
			}
		}
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

#define SYS_chown               16
#define SYS_umask               60

/* [MODE]: the mask of permissions a new file is made without (octal), set if given; the one there was is said */
void
command_umask(int argc, char **argv)
{
	unsigned mode = 0;
	const char *c = argc > 1 ? argv[1] : "";
	long was;

	for (; *c >= '0' && *c <= '7'; c++) {
		mode = mode * 8 + (unsigned)(*c - '0');
	}
	if (argc > 2 || *c != '\0' || mode > 0777) {
		say("umask [MODE] (MODE in octal)\n");
		return;
	}
	was = syscall6(SYS_umask, (long)mode, 0, 0, 0, 0, 0);
	if (argc == 1) {
		syscall6(SYS_umask, was, 0, 0, 0, 0, 0);        /* (only looked at) */
	}
	say("umask: ");
	say_number((uint64_t)(was >> 6 & 7));
	say_number((uint64_t)(was >> 3 & 7));
	say_number((uint64_t)(was & 7));
	say(argc > 1 ? " before\n" : "\n");
}

/* UID:GID PATH...: whose each is */
void
command_chown(int argc, char **argv)
{
	unsigned uid = 0, gid = 0;
	const char *c = argc > 1 ? argv[1] : "";

	for (; *c >= '0' && *c <= '9'; c++) {
		uid = uid * 10 + (unsigned)(*c - '0');
	}
	if (*c == ':') {
		for (c++; *c >= '0' && *c <= '9'; c++) {
			gid = gid * 10 + (unsigned)(*c - '0');
		}
	}
	if (argc < 3 || *c != '\0') {
		say("chown UID:GID PATH...\n");
		return;
	}
	for (int i = 2; i < argc; i++) {
		long error = syscall6(SYS_chown, (long)argv[i], uid, gid, 0, 0, 0);
		if (error != 0) {
			say_failed("chown", argv[i], error);
		}
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
		uint64_t hash = FNV1A_START, all = 0;
		int64_t start = now_us();
		for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += (uint64_t)got) {
			hash = fnv1a(hash, buffer, got);
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


#define SYS_dup                 41
#define SYS_fcntl               92
#define SYS_dup2                90
#define SYS_pread               153
#define SYS_pwrite              154
#define SYS_lseek               199
#define SYS_fstat64             339
#define O_RDWR                  2
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
 * end whatever the offset, a file read and written after its name is taken away (unlink) until it is closed,
 * and a non-blocking pipe with nothing in it saying EAGAIN
 */
void
command_descriptors(int argc, char **argv)
{
	char line[400], two[2];
	size_t at;
	long fd, copy, pipe_result;
	int ends[2];
	struct { const char *what; int ok; } said[8];
	struct stat64_head st;
	int n = 0, all = 1;

	if (argc != 2) {
		say("descriptors FILE (made anew, and taken away)\n");
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
	/* (the file's name taken away while it is open: the file lives on, nameless, until it is closed) */
	said[n].what = "a file open after its unlink";
	said[n++].ok = fd >= 0 && syscall6(SYS_unlink, (long)argv[1], 0, 0, 0, 0, 0) == 0 &&
	    syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0) == -2 &&
	    syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) == 0 && st.nlink == 0 && st.size == 11 &&
	    syscall6(SYS_pwrite, fd, (long)"?", 1, 11, 0, 0) == 1 && syscall6(SYS_pread, fd, (long)two, 2, 10, 0, 0) == 2 &&
	    bytes_same(two, "!?", 2);
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

#define SYS_truncate            200
#define SYS_utimes              138

/* FILE BYTES: the file cut to BYTES, or grown to it (truncate); what it grew by read back, which is zeros (of a file
 * grown by more than 128 KB, the first and last 64 KB of it) */
void
command_truncate(int argc, char **argv)
{
	struct stat64_head st;
	char buffer[512], line[200];
	uint64_t bytes = argc == 3 ? parse_number64(argv[2]) : 0;
	uint64_t was, nonzero = 0;
	size_t at;
	long error, fd;

	if (argc != 3 || (bytes == 0 && !same(argv[2], "0"))) {
		say("truncate FILE BYTES\n");
		return;
	}
	if ((error = syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0)) != 0 ||
	    (was = (uint64_t)st.size, error = syscall6(SYS_truncate, (long)argv[1], bytes, 0, 0, 0, 0)) != 0 ||
	    (error = syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0)) != 0) {
		say_failed("truncate", argv[1], error);
		return;
	}
	at = append(line, 0, sizeof(line), "truncate: ");
	at = append(line, at, sizeof(line), argv[1]);
	at = append(line, at, sizeof(line), ": ");
	at = append_number(line, at, sizeof(line), was);
	at = append(line, at, sizeof(line), " bytes, now ");
	at = append_number(line, at, sizeof(line), (uint64_t)st.size);
	if ((uint64_t)st.size > was && (fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0)) >= 0) {
		/* (of a file grown far, its first and last 64 KB of what it grew by: reading gigabytes would take minutes) */
		uint64_t size = (uint64_t)st.size, off = was, read = 0, skip_to = size - was > 131072 ? size - 65536 : size;
		long got = 1;

		while (off < size && (got = syscall6(SYS_pread, fd, (long)buffer, sizeof(buffer), (long)off, 0, 0)) > 0) {
			for (long i = 0; i < got; i++) {
				nonzero += buffer[i] != 0;
			}
			off += (uint64_t)got;
			read += (uint64_t)got;
			if (off >= was + 65536 && off < skip_to) {
				off = skip_to;
			}
		}
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		at = append(line, at, sizeof(line), off < size ? "; what it grew by does NOT all read back" :
		    nonzero == 0 ? "; what it grew by reads as zeros" : "; what it grew by is NOT all zeros");
		if (read < size - was) {
			at = append(line, at, sizeof(line), " (its first and last 64 KB)");
		}
	}
	at = append(line, at, sizeof(line), "\n");
	say(line);
}

/* FILE [SECONDS]: the file made if it is not there, and its times set: to now, or to SECONDS since 1970 (utimes);
 * the times it has then */
void
command_touch(int argc, char **argv)
{
	struct { int64_t sec; int32_t usec, pad; } times[2];
	struct stat64_head st;
	char line[200];
	size_t at;
	long error, fd;

	if (argc < 2 || argc > 3 || (argc == 3 && parse_number(argv[2]) == 0 && !same(argv[2], "0"))) {
		say("touch FILE [SECONDS]\n");
		return;
	}
	if ((fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT, 0666, 0, 0, 0)) < 0) {
		say_failed("touch", argv[1], fd);
		return;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (argc == 3) {
		times[0].sec = times[1].sec = parse_number(argv[2]);
		times[0].usec = times[1].usec = times[0].pad = times[1].pad = 0;
	}
	if ((error = syscall6(SYS_utimes, (long)argv[1], argc == 3 ? (long)times : 0, 0, 0, 0, 0)) != 0 ||
	    (error = syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0)) != 0) {
		say_failed("touch", argv[1], error);
		return;
	}
	/* (struct stat's times: accessed, modified, changed, born, each seconds and nanoseconds) */
	at = append(line, 0, sizeof(line), "touch: ");
	at = append(line, at, sizeof(line), argv[1]);
	at = append(line, at, sizeof(line), ": accessed ");
	at = append_number(line, at, sizeof(line), (uint64_t)st.times[0]);
	at = append(line, at, sizeof(line), ", modified ");
	at = append_number(line, at, sizeof(line), (uint64_t)st.times[2]);
	at = append(line, at, sizeof(line), ", changed ");
	at = append_number(line, at, sizeof(line), (uint64_t)st.times[4]);
	at = append(line, at, sizeof(line), "\n");
	say(line);
}

#define SYS_readv               120
#define SYS_writev              121

/* FILE: a file made anew from three pieces in one writev, read back in one readv into pieces of other sizes */
void
command_vectors(int argc, char **argv)
{
	static const char whole[] = "one piece, then another, then the last";
	struct { const void *base; uint64_t len; } out[3] = {
		{ whole, 10 }, { whole + 10, 14 }, { whole + 24, sizeof(whole) - 1 - 24 },
	};
	char a[5], b[20], c[40];
	struct { void *base; uint64_t len; } in[3] = { { a, sizeof(a) }, { b, sizeof(b) }, { c, sizeof(c) } };
	char back[sizeof(a) + sizeof(b) + sizeof(c)], line[200];
	long fd, wrote, read;
	size_t at;

	if (argc != 2) {
		say("vectors FILE\n");
		return;
	}
	syscall6(SYS_unlink, (long)argv[1], 0, 0, 0, 0, 0);
	if ((fd = syscall6(SYS_open, (long)argv[1], O_RDWR | O_CREAT, 0644, 0, 0, 0)) < 0) {
		say_failed("vectors", argv[1], fd);
		return;
	}
	wrote = syscall6(SYS_writev, fd, (long)out, 3, 0, 0, 0);
	syscall6(SYS_lseek, fd, 0, 0, 0, 0, 0);
	read = syscall6(SYS_readv, fd, (long)in, 3, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	copy_bytes(back, a, sizeof(a));
	copy_bytes(back + sizeof(a), b, sizeof(b));
	copy_bytes(back + sizeof(a) + sizeof(b), c, sizeof(c));
	at = append(line, 0, sizeof(line), "vectors: ");
	at = append(line, at, sizeof(line), argv[1]);
	at = append(line, at, sizeof(line), ": ");
	at = append_number(line, at, sizeof(line), wrote < 0 ? 0 : (uint64_t)wrote);
	at = append(line, at, sizeof(line), " bytes written from 3 pieces, ");
	at = append_number(line, at, sizeof(line), read < 0 ? 0 : (uint64_t)read);
	at = append(line, at, sizeof(line), " read into 3 others");
	at = append(line, at, sizeof(line), read == (long)sizeof(whole) - 1 && bytes_same(back, whole, sizeof(whole) - 1) ?
	    ", in order\n" : ", NOT as written\n");
	say(line);
}

#define SYS_chflags             34

/* FLAGS PATH...: a file's flags (chflags), FLAGS in hexadecimal: 2 UF_IMMUTABLE, 4 UF_APPEND, 0x20000 SF_IMMUTABLE */
void
command_chflags(int argc, char **argv)
{
	uint32_t flags = 0;
	const char *c = argc > 1 ? argv[1] : "";

	if (c[0] == '0' && c[1] == 'x') {
		c += 2;
	}
	for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++) {
		flags = flags * 16 + (uint32_t)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
	}
	if (argc < 3 || *c != '\0') {
		say("chflags FLAGS PATH... (FLAGS in hexadecimal)\n");
		return;
	}
	for (int i = 2; i < argc; i++) {
		long error = syscall6(SYS_chflags, (long)argv[i], flags, 0, 0, 0, 0);
		if (error != 0) {
			say_failed("chflags", argv[i], error);
		}
	}
}

#define SYS_access              33

/* PATH...: what this process may do with each, as access() says: read, write, run (or search, for a directory) */
void
command_access(int argc, char **argv)
{
	static const struct { int mode; const char *what; } asks[] = { { 4, "read" }, { 2, "write" }, { 1, "run" } };

	if (argc < 2) {
		say("access PATH...\n");
		return;
	}
	for (int i = 1; i < argc; i++) {
		char line[200];
		size_t at = append(line, 0, sizeof(line), "access: ");
		long exists = syscall6(SYS_access, (long)argv[i], 0 /* F_OK */, 0, 0, 0, 0);

		at = append(line, at, sizeof(line), argv[i]);
		if (exists != 0) {
			say(line);
			say(": ");
			say_error(exists);
			say("\n");
			continue;
		}
		for (unsigned a = 0; a < 3; a++) {
			long error = syscall6(SYS_access, (long)argv[i], asks[a].mode, 0, 0, 0, 0);

			at = append(line, at, sizeof(line), a == 0 ? ": " : ", ");
			at = append(line, at, sizeof(line), asks[a].what);
			at = append(line, at, sizeof(line), error == 0 ? " yes" : error == -13 ? " no" : error == -30 ? " no (EROFS)" :
			    error == -1 ? " no (EPERM)" : " no (error ");
			if (error != 0 && error != -13 && error != -30 && error != -1) {
				at = append(line, append_number(line, at, sizeof(line), (uint64_t)-error), sizeof(line), ")");
			}
		}
		append(line, at, sizeof(line), "\n");
		say(line);
	}
}

/* SOURCE TARGET: the file's bytes into a new file of the same permissions */
void
command_cp(int argc, char **argv)
{
	static char buffer[16384];
	struct stat64_head st;
	long from, to, got, error;

	if (argc != 3) {
		say("cp SOURCE TARGET\n");
		return;
	}
	if ((error = syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0)) != 0 ||
	    (from = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0)) < 0) {
		say_failed("cp", argv[1], error != 0 ? error : from);
		return;
	}
	if ((to = syscall6(SYS_open, (long)argv[2], O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777, 0, 0, 0)) < 0) {
		say_failed("cp", argv[2], to);
		syscall6(SYS_close, from, 0, 0, 0, 0, 0);
		return;
	}
	while ((got = syscall6(SYS_read, from, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0) {
		if (syscall6(SYS_write, to, (long)buffer, got, 0, 0, 0) != got) {
			say("cp: ");
			say(argv[2]);
			say(": not all written\n");
			break;
		}
	}
	syscall6(SYS_close, from, 0, 0, 0, 0, 0);
	syscall6(SYS_close, to, 0, 0, 0, 0, 0);
}

#define RLIMIT_FSIZE            1
#define SYS_wait4               7
#define SYS_sigaction           46
#define SIGXFSZ                 25

/*
 * FILE BYTES: in a child whose files may be no larger than BYTES (setrlimit RLIMIT_FSIZE), SIGXFSZ ignored: the
 * file written anew with twice BYTES (cut at BYTES), written again past them (EFBIG), and cut to more (EFBIG)
 */
void
command_fsize(int argc, char **argv)
{
	static char bytes[8192];
	struct { uint64_t current, most; } limit;
	struct { long handler, trampoline; uint32_t mask; int32_t flags; } ignore = { 1 /* SIG_IGN */, 0, 0, 0 };
	unsigned most = argc == 3 ? parse_number(argv[2]) : 0;
	int in_child = 0;
	int32_t status = 0;
	long pid;

	if (argc != 3 || most == 0 || most > sizeof(bytes) / 2) {
		say("fsize FILE BYTES (1 to 4096)\n");
		return;
	}
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("fsize: no child\n");
		return;
	}
	if (in_child) {
		char line[200];
		size_t at;
		long fd = -1, first, second, cut;      /* (fd: the compiler does not know that exit does not return) */

		limit.current = limit.most = most;
		if (syscall6(SYS_sigaction, SIGXFSZ, (long)&ignore, 0, 0, 0, 0) != 0 ||
		    syscall6(SYS_setrlimit, RLIMIT_FSIZE, (long)&limit, 0, 0, 0, 0) != 0 ||
		    (fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0)) < 0) {
			say("fsize: the limit cannot be had, or the file\n");
			syscall6(SYS_exit, 1, 0, 0, 0, 0, 0);
		}
		first = syscall6(SYS_write, fd, (long)bytes, 2 * most, 0, 0, 0);
		second = syscall6(SYS_write, fd, (long)bytes, 1, 0, 0, 0);
		cut = syscall6(SYS_truncate, (long)argv[1], most + 1, 0, 0, 0, 0);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		at = append(line, 0, sizeof(line), "fsize: ");
		at = append(line, at, sizeof(line), argv[1]);
		at = append(line, at, sizeof(line), ": ");
		at = append_number(line, at, sizeof(line), (uint64_t)(first < 0 ? 0 : first));
		at = append(line, at, sizeof(line), " of ");
		at = append_number(line, at, sizeof(line), 2 * most);
		at = append(line, at, sizeof(line), " written; past the limit, a write ");
		at = append(line, at, sizeof(line), second == -27 ? "EFBIG" : "NOT EFBIG");
		at = append(line, at, sizeof(line), ", a truncate ");
		at = append(line, at, sizeof(line), cut == -27 ? "EFBIG\n" : "NOT EFBIG\n");
		say(line);
		syscall6(SYS_exit, 0, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
}

/*
 * FILE COUNT: this process and a child each add COUNT records of 1000 bytes to the file (O_APPEND) at the same
 * time; where each write starts is the end of the file as it is then, so every record must end up whole, none
 * written over another, and the file 2000 * COUNT bytes long
 */
void
command_appends(int argc, char **argv)
{
	static char record[1000];
	struct stat64_head st;
	unsigned count = argc == 3 ? parse_number(argv[2]) : 0;
	uint64_t whole[2] = { 0, 0 }, torn = 0;
	int in_child = 0;
	int32_t status = 0;
	char line[200];
	size_t at;
	long fd, pid, error;

	if (argc != 3 || count == 0 || count > 100000) {
		say("appends FILE COUNT (1 to 100000)\n");
		return;
	}
	if ((fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0)) < 0) {
		say_failed("appends", argv[1], fd);
		return;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("appends: no child\n");
		return;
	}
	for (size_t i = 0; i < sizeof(record); i++) {
		record[i] = in_child ? 'b' : 'a';
	}
	if ((fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_APPEND, 0, 0, 0, 0)) >= 0) {
		for (unsigned i = 0; i < count; i++) {
			if (syscall6(SYS_write, fd, (long)record, sizeof(record), 0, 0, 0) != (long)sizeof(record)) {
				break;
			}
		}
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	if (in_child) {
		syscall6(SYS_exit, 0, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);

	if ((error = syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0)) != 0 ||
	    (error = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0)) < 0) {
		say_failed("appends", argv[1], error);
		return;
	}
	fd = error;
	for (uint64_t off = 0; off + sizeof(record) <= (uint64_t)st.size; off += sizeof(record)) {
		size_t same_bytes = 0;

		if (syscall6(SYS_pread, fd, (long)record, sizeof(record), (long)off, 0, 0) != (long)sizeof(record)) {
			torn++;
			break;
		}
		for (size_t i = 0; i < sizeof(record); i++) {
			same_bytes += record[i] == record[0];
		}
		if (same_bytes == sizeof(record) && (record[0] == 'a' || record[0] == 'b')) {
			whole[record[0] == 'b']++;
		} else {
			torn++;
		}
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "appends: ");
	at = append(line, at, sizeof(line), argv[1]);
	at = append(line, at, sizeof(line), ": ");
	at = append_number(line, at, sizeof(line), (uint64_t)st.size);
	at = append(line, at, sizeof(line), " bytes of ");
	at = append_number(line, at, sizeof(line), 2 * (uint64_t)count * sizeof(record));
	at = append(line, at, sizeof(line), "; whole records ");
	at = append_number(line, at, sizeof(line), whole[0]);
	at = append(line, at, sizeof(line), " of this process and ");
	at = append_number(line, at, sizeof(line), whole[1]);
	at = append(line, at, sizeof(line), " of the child, ");
	at = append_number(line, at, sizeof(line), torn);
	at = append(line, at, sizeof(line), " torn\n");
	say(line);
}

#define O_EXCL                  0x800
#define O_NOFOLLOW              0x100

/* FILE (made anew, and a symbolic link to it beside it, FILE-link): what open says as BSD has it -- O_EXCL of one
 * that is there, O_NOFOLLOW of a link, O_DIRECTORY of a file, a directory opened to write */
void
command_opens(int argc, char **argv)
{
	char link[256], line[300];
	struct { const char *what; long error, wanted; } said[5];
	size_t at, n = 0;
	long fd;
	int all = 1;

	if (argc != 2 || length(argv[1]) > sizeof(link) - 6) {
		say("opens FILE (made anew)\n");
		return;
	}
	at = append(link, 0, sizeof(link), argv[1]);
	append(link, at, sizeof(link), "-link");
	syscall6(SYS_unlink, (long)argv[1], 0, 0, 0, 0, 0);
	syscall6(SYS_unlink, (long)link, 0, 0, 0, 0, 0);
	if ((fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0, 0)) < 0 ||
	    syscall6(SYS_symlink, (long)argv[1], (long)link, 0, 0, 0, 0) != 0) {
		say_failed("opens", argv[1], fd < 0 ? fd : -1);
		return;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	said[n].what = "O_EXCL of one there";
	said[n].wanted = -17;
	said[n++].error = syscall6(SYS_open, (long)argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0, 0);
	said[n].what = "O_NOFOLLOW of a link";
	said[n].wanted = -62;
	said[n++].error = syscall6(SYS_open, (long)link, O_RDONLY | O_NOFOLLOW, 0, 0, 0, 0);
	said[n].what = "O_DIRECTORY of a file";
	said[n].wanted = -20;
	said[n++].error = syscall6(SYS_open, (long)argv[1], O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);
	said[n].what = "a directory to write";
	said[n].wanted = -21;
	said[n++].error = syscall6(SYS_open, (long)"/tmp", O_WRONLY, 0, 0, 0, 0);
	said[n].what = "through the link";
	said[n].wanted = 0;
	fd = syscall6(SYS_open, (long)link, O_RDONLY, 0, 0, 0, 0);
	said[n++].error = fd >= 0 ? 0 : fd;
	if (fd >= 0) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	at = append(line, 0, sizeof(line), "opens:");
	for (size_t i = 0; i < n; i++) {
		int ok = said[i].error == said[i].wanted;

		if (!ok && said[i].error >= 0 && said[i].wanted != 0) {
			syscall6(SYS_close, said[i].error, 0, 0, 0, 0, 0);      /* (opened after all) */
		}
		all &= ok;
		at = append(line, at, sizeof(line), " ");
		at = append(line, at, sizeof(line), said[i].what);
		at = append(line, at, sizeof(line), ok ? "," : " WRONG,");
	}
	append(line, at - 1, sizeof(line), all ? ": as BSD has them\n" : ": NOT all as BSD has them\n");
	say(line);
	syscall6(SYS_unlink, (long)link, 0, 0, 0, 0, 0);
}

/* DIR's entries from where `base' is, read in `room' bytes at a time; each name given to `each' (not . or ..) */
static long
walk(long fd, int64_t *base, size_t room, void (*each)(const char *dir, const char *name), const char *dir)
{
	char buffer[4096];
	long names = 0, got = syscall6(SYS_getdirentries64, fd, (long)buffer, (long)room, (long)base, 0, 0);

	for (long at = 0; at < got;) {
		uint16_t reclen = *(uint16_t *)(buffer + at + 16);
		const char *name = buffer + at + 21;

		if (!(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))) {
			names++;
			if (each != NULL) {
				each(dir, name);
			}
		}
		at += reclen;
	}
	return got <= 0 ? -1 : names;
}

static void
remove_one(const char *dir, const char *name)
{
	char path[512];

	append(path, append(path, append(path, 0, sizeof(path), dir), sizeof(path), "/"), sizeof(path), name);
	syscall6(SYS_unlink, (long)path, 0, 0, 0, 0, 0);
}

/*
 * DIR COUNT: COUNT files made in DIR (made anew); its entries read 512 bytes at a time, all of them once;
 * then read again and each batch removed as it comes, as a program that empties a directory does: none is
 * passed over, and the directory is empty at the end
 */
void
command_dir_walk(int argc, char **argv)
{
	unsigned count = argc == 3 ? parse_number(argv[2]) : 0;
	char path[512], line[200];
	long fd, n, seen = 0, removed = 0, calls = 0, left;
	int64_t base = 0;
	size_t at;

	if (count == 0 || count > 1000 || length(argv[1]) > 400) {
		say("dir-walk DIR COUNT (1 to 1000)\n");
		return;
	}
	if ((n = syscall6(SYS_mkdir, (long)argv[1], 0755, 0, 0, 0, 0)) != 0) {
		say_failed("dir-walk", argv[1], n);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		at = append(path, 0, sizeof(path), argv[1]);
		at = append(path, at, sizeof(path), "/entry-");
		append_number(path, at, sizeof(path), i);
		if ((fd = syscall6(SYS_open, (long)path, O_WRONLY | O_CREAT, 0644, 0, 0, 0)) >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
	}
	fd = syscall6(SYS_open, (long)argv[1], O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);
	while ((n = walk(fd, &base, 512, NULL, NULL)) >= 0) {
		seen += n;
		calls++;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	fd = syscall6(SYS_open, (long)argv[1], O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);
	base = 0;
	while ((n = walk(fd, &base, 512, remove_one, argv[1])) >= 0) {
		removed += n;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	left = syscall6(SYS_rmdir, (long)argv[1], 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "dir-walk: ");
	at = append_number(line, at, sizeof(line), count);
	at = append(line, at, sizeof(line), " made, ");
	at = append_number(line, at, sizeof(line), (uint64_t)seen);
	at = append(line, at, sizeof(line), " read in ");
	at = append_number(line, at, sizeof(line), (uint64_t)calls);
	at = append(line, at, sizeof(line), " reads; removed as read: ");
	at = append_number(line, at, sizeof(line), (uint64_t)removed);
	at = append(line, at, sizeof(line), left == 0 ? ", the directory then empty\n" : ", the directory NOT empty\n");
	say(line);
}
