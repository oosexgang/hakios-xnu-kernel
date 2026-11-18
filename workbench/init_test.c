/*
 * Host tests of process 1 (init/init.c), as it is: the preferences files it writes into mobile's directory
 * after the store has put back the last boot's, the 1970 date it takes out of dasd's, the lines of a job list it
 * runs, its directory walks and the numbers it reads from a boot argument or a job line.
 *
 * As with var_store_test.c, the program is compiled against libmini's header and the host's C library
 * answers the calls it declares. What must not reach the host is the test's: a path under /private (or
 * /jb) is taken to a directory of the test's, with Darwin's open flags turned into Linux's; fork, execve
 * and wait4 only note what they were asked, and so do usleep, settimeofday and mount; gettimeofday and
 * getdirentries hand out what a scenario wants; sysctlbyname answers with a boot line and a kernel version of
 * the test's choosing. What init says is kept, to be looked at.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"  /* (libmini's strerror gives a const string) */
#endif
#define main init_main
#define errno init_errno                /* (the host's errno is not a plain int) */
#define printf(...) t_printf(__VA_ARGS__)
#define open t_open
#define unlink t_unlink
#define stat(path, st) t_stat(path, st)  /* (a macro with arguments: struct stat stays what it is) */
#define lstat t_lstat
#define chown t_chown
#define chmod t_chmod
#define mkdir t_mkdir
#define symlink t_symlink
#define getdirentries t_getdirentries
#define fork t_fork
#define execve t_execve
#define wait4 t_wait4
#define usleep t_usleep
#define settimeofday t_settimeofday
#define gettimeofday t_gettimeofday
#define mount t_mount
#include "../init/init.c"
#undef main
#undef errno
#undef printf
#undef open
#undef unlink
#undef stat
#undef lstat
#undef chown
#undef chmod
#undef mkdir
#undef symlink
#undef getdirentries
#undef fork
#undef execve
#undef wait4
#undef usleep
#undef settimeofday
#undef gettimeofday
#undef mount

int init_errno;

/* the host's own, which mini.h does not declare (or declares under the names above) */
extern int *__errno_location(void);
int     printf(const char *format, ...);
int     snprintf(char *buf, size_t size, const char *format, ...);
int     vsnprintf(char *buf, size_t size, const char *format, va_list ap);
char   *strstr(const char *haystack, const char *needle);
char   *getenv(const char *name);
int     system(const char *command);
int     open(const char *path, int flags, ...);
int     unlink(const char *path);
int     chown(const char *path, unsigned uid, unsigned gid);
int     chmod(const char *path, unsigned mode);
int     mkdir(const char *path, unsigned mode);
int     symlink(const char *target, const char *path);
int     fork(void);
int     waitpid(int pid, int *status, int options);
unsigned alarm(unsigned seconds);
void  (*signal(int number, void (*handler)(int)))(int);
int     utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);

#define HOST_ERRNO      (*__errno_location())
#define A_DAY           86400           /* the longest sleep a job line gets, in seconds */
#define HOST_SIGALRM    14

/* Linux's open flags, not Darwin's: the file is opened by the host's C library */
#define HOST_O_CREAT    0x40
#define HOST_O_EXCL     0x80
#define HOST_O_TRUNC    0x200
#define HOST_O_APPEND   0x400
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

static int t_failures, t_checks;
#define CHECK(cond, ...) do { t_checks++; if (!(cond)) { t_failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what init asks of the system, the test's way ---- */

static char root[512];                  /* the test's directory: /private and /jb are in it */
static const char *boot_line = "";
static const char *kernel_version;      /* kern.version, when a scenario gives one */
static long long rtc_answer = -1;       /* hw.rtc_seconds, when a scenario gives one (the PMIC clock's count) */

/* PATH as the host is to see it */
static const char *
host_path(const char *path)
{
	static char buf[4][4096];
	static int next;
	char *out = buf[next++ % 4];

	if (strncmp(path, "/private/", 9) == 0 || strcmp(path, "/private") == 0 || strncmp(path, "/jb/", 4) == 0 ||
	    strcmp(path, "/jb") == 0) {
		snprintf(out, sizeof(buf[0]), "%s%s", root, path);
		return out;
	}
	return path;
}

static int
host_flags(int flags)
{
	int host = flags & 3;

	host |= flags & O_CREAT ? HOST_O_CREAT : 0;
	host |= flags & O_EXCL ? HOST_O_EXCL : 0;
	host |= flags & O_TRUNC ? HOST_O_TRUNC : 0;
	host |= flags & O_APPEND ? HOST_O_APPEND : 0;
	host |= flags & O_DIRECTORY ? HOST_O_DIRECTORY : 0;
	host |= flags & O_NOFOLLOW ? HOST_O_NOFOLLOW : 0;
	return host;
}

/* the result of a host call, its errno where init looks for it */
static int
answer(int result)
{
	if (result < 0) {
		init_errno = HOST_ERRNO;
	}
	return result;
}

static char said[1 << 20];              /* everything init printed */
static size_t said_length;

int
t_printf(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(said + said_length, sizeof(said) - said_length, format, ap);
	va_end(ap);
	if (n > 0) {
		said_length += (size_t)n < sizeof(said) - said_length ? (size_t)n : sizeof(said) - said_length - 1;
	}
	return n;
}

static int
init_said(const char *text)
{
	return strstr(said, text) != NULL;
}

static void
forget_said(void)
{
	said_length = 0;
	said[0] = 0;
}

int
t_open(const char *path, int flags, int mode)
{
	return answer(open(host_path(path), host_flags(flags), mode));
}

int
t_unlink(const char *path)
{
	return answer(unlink(host_path(path)));
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

static int
host_statx(const char *path, int follow, struct host_statx *x)
{
	return statx(HOST_AT_FDCWD, path, follow ? 0 : HOST_AT_SYMLINK_NOFOLLOW, HOST_STATX_BASIC, x);
}

int
t_stat(const char *path, struct stat *st)
{
	struct host_statx x;

	if (answer(host_statx(host_path(path), 1, &x)) != 0) {
		return -1;
	}
	from_statx(&x, st);
	return 0;
}

int
t_lstat(const char *path, struct stat *st)
{
	struct host_statx x;

	if (answer(host_statx(host_path(path), 0, &x)) != 0) {
		return -1;
	}
	from_statx(&x, st);
	return 0;
}

int
t_chown(const char *path, int uid, int gid)
{
	return answer(chown(host_path(path), (unsigned)uid, (unsigned)gid));
}

int
t_chmod(const char *path, int mode)
{
	return answer(chmod(host_path(path), (unsigned)mode));
}

int
t_mkdir(const char *path, int mode)
{
	return answer(mkdir(host_path(path), (unsigned)mode));
}

int
t_symlink(const char *target, const char *path)
{
	return answer(symlink(target, host_path(path)));
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
	if (strcmp(name, "kern.version") == 0 && kernel_version != NULL && oldp != NULL && oldlenp != NULL) {
		size_t n = strlen(kernel_version) + 1;

		if (n > *oldlenp) {
			init_errno = 12;        /* ENOMEM, as the kernel answers a buffer too small */
			return -1;
		}
		memcpy(oldp, kernel_version, n);
		*oldlenp = n;
		return 0;
	}
	if (strcmp(name, "hw.rtc_seconds") == 0 && rtc_answer >= 0 && oldp != NULL && oldlenp != NULL &&
	    *oldlenp >= sizeof(uint64_t)) {
		uint64_t count = (uint64_t)rtc_answer;

		memcpy(oldp, &count, sizeof(count));
		*oldlenp = sizeof(count);
		return 0;
	}
	init_errno = 2;
	return -1;
}

/* a directory with one record in it, ".", whose length is 0, handed out once when a scenario asks (a name a walk passes
 * over, so that nothing but the length can end it) */
static int zero_record_armed;

ssize_t
t_getdirentries(int fd, void *buf, size_t size, off_t *position)
{
	struct dirent de;
	size_t n = offsetof(struct dirent, d_name) + 8;

	(void)fd;
	if (!zero_record_armed || size < n) {
		return 0;
	}
	zero_record_armed = 0;
	memset(&de, 0, sizeof(de));
	de.d_ino = 99;
	de.d_reclen = 0;
	de.d_namlen = 1;
	de.d_type = DT_REG;
	de.d_name[0] = '.';
	memcpy(buf, &de, n);
	*position += 1;
	return (ssize_t)n;
}

static int forks, waits_blocking, waited_for, execs, exec_stderr_was_null, sleeps, clock_sets;
static long clock_set_to, clock_now;
static char exec_path[256];
static struct host_statx dev_null;

pid_t
t_fork(void)
{
	return 4000 + ++forks;          /* (the parent's side: no child is made) */
}

pid_t
t_wait4(pid_t pid, int *status, int options, void *rusage)
{
	(void)rusage;
	if (options & WNOHANG) {
		return 0;
	}
	waits_blocking++;
	waited_for = pid;
	if (status != NULL) {
		*status = 0;
	}
	return pid;
}

int
t_execve(const char *path, char *const argv[], char *const envp[])
{
	struct host_statx x;

	(void)argv;
	(void)envp;
	execs++;
	snprintf(exec_path, sizeof(exec_path), "%s", path);
	exec_stderr_was_null = statx(2, "", HOST_AT_EMPTY_PATH, HOST_STATX_BASIC, &x) == 0 && x.ino == dev_null.ino &&
	    x.dev_major == dev_null.dev_major && x.dev_minor == dev_null.dev_minor;
	init_errno = 2;
	return -1;
}

int
t_usleep(unsigned microseconds)
{
	(void)microseconds;
	sleeps++;
	return 0;
}

int
t_settimeofday(const struct timeval *tv, const void *tz)
{
	(void)tz;
	clock_sets++;
	clock_set_to = tv->tv_sec;
	return 0;
}

int
t_gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	tv->tv_sec = clock_now;
	tv->tv_usec = 0;
	return 0;
}

int
t_mount(const char *type, const char *dir, int flags, void *data)
{
	(void)type;
	(void)dir;
	(void)flags;
	(void)data;
	return 0;
}

static void
forget_calls(void)
{
	forks = waits_blocking = waited_for = execs = exec_stderr_was_null = sleeps = clock_sets = 0;
	clock_set_to = 0;
	exec_path[0] = 0;
	forget_said();
}

/* ---- files of the test's, on the host ---- */

static const char *
in_root(const char *path)
{
	static char buf[4][4096];
	static int next;
	char *out = buf[next++ % 4];

	snprintf(out, sizeof(buf[0]), "%s%s", root, path);
	return out;
}

static void
put_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | HOST_O_CREAT | HOST_O_TRUNC, 0600);

	CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text), "%s written", path);
	close(fd);
}

/* the file's contents, up to 4 KB, or "" */
static const char *
file_text(const char *path)
{
	static char buf[4096];
	int fd = open(path, O_RDONLY, 0);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

	buf[n > 0 ? n : 0] = 0;
	if (fd >= 0) {
		close(fd);
	}
	return buf;
}

/* ---- the scenarios ---- */

#define PREFS           "/private/var/mobile/Library/Preferences"
#define DARK_PLIST      PREFS "/com.apple.uikitservices.userInterfaceStyleMode.plist"
#define BT_PLIST        PREFS "/com.apple.MobileBluetooth.debug.plist"

/* A preferences file is written where it is named, never through a link that the store put back. */
static void
scenario_preferences(void)
{
	static const char *const dirs[] = { "/private", "/private/var", "/private/var/mobile", "/private/var/mobile/Library",
		                            PREFS, "/private/var/root" };
	const char *own = "root's own, not to be changed\n";
	struct host_statx x;
	int as_root = getuid() == 0;

	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		mkdir(in_root(dirs[i]), 0755);
	}
	/* a symbolic link in one plist's place, a second name of a file of root's in the other's */
	put_file(in_root("/private/var/root/secret"), own);
	put_file(in_root("/private/var/root/hard"), own);
	CHECK(symlink(in_root("/private/var/root/secret"), in_root(DARK_PLIST)) == 0, "the symbolic link planted");
	CHECK(link(in_root("/private/var/root/hard"), in_root(BT_PLIST)) == 0, "the hard link planted");

	forget_calls();
	boot_line = "rd=md0 dark=1 btnolpcore=1";
	boot_arg_preferences();
	CHECK(strcmp(file_text(in_root("/private/var/root/secret")), own) == 0, "the file the symbolic link led to is as it was: \"%s\"",
	    file_text(in_root("/private/var/root/secret")));
	CHECK(strcmp(file_text(in_root("/private/var/root/hard")), own) == 0, "the file of the hard link is as it was: \"%s\"",
	    file_text(in_root("/private/var/root/hard")));
	CHECK(host_statx(in_root("/private/var/root/hard"), 0, &x) == 0 && x.nlink == 1, "and has no other name now");
	if (as_root) {
		CHECK(host_statx(in_root("/private/var/root/secret"), 0, &x) == 0 && x.uid == 0 && x.gid == 0,
		    "root's file still belongs to root: %u:%u", x.uid, x.gid);
		CHECK(host_statx(in_root("/private/var/root/hard"), 0, &x) == 0 && x.uid == 0 && x.gid == 0,
		    "root's second file still belongs to root: %u:%u", x.uid, x.gid);
	}
	for (int i = 0; i < 2; i++) {
		const char *plist = in_root(i == 0 ? DARK_PLIST : BT_PLIST);

		CHECK(host_statx(plist, 0, &x) == 0 && (x.mode & S_IFMT) == S_IFREG && (x.mode & 07777) == 0600 && x.nlink == 1,
		    "%s is a file of its own, mode 0600: mode %o, %u links", plist, x.mode, x.nlink);
		CHECK(strncmp(file_text(plist), "<?xml", 5) == 0, "%s holds the plist", plist);
		if (as_root) {
			CHECK(x.uid == 501 && x.gid == 501, "%s belongs to mobile: %u:%u", plist, x.uid, x.gid);
		}
	}
	CHECK(strstr(file_text(in_root(DARK_PLIST)), "UserInterfaceStyleMode") != NULL, "the dark appearance's plist");
	CHECK(init_said("dark appearance: set") && init_said("low-power-core scan off: set"), "both said to be set: %s", said);
	if (!as_root) {
		printf("init_test: not root here: who owns the files is not looked at\n");
	}

	/* the directory on the way a link to another of root's: nothing is written there */
	CHECK(unlink(in_root(DARK_PLIST)) == 0 && unlink(in_root(BT_PLIST)) == 0 && rmdir(in_root(PREFS)) == 0, "Preferences emptied");
	put_file(in_root("/private/var/root/com.apple.uikitservices.userInterfaceStyleMode.plist"), own);
	CHECK(symlink(in_root("/private/var/root"), in_root(PREFS)) == 0, "Preferences a link to root's directory");
	forget_calls();
	boot_line = "dark=1";
	boot_arg_preferences();
	CHECK(strcmp(file_text(in_root("/private/var/root/com.apple.uikitservices.userInterfaceStyleMode.plist")), own) == 0,
	    "root's file of the plist's name, through a linked directory, is as it was: \"%s\"",
	    file_text(in_root("/private/var/root/com.apple.uikitservices.userInterfaceStyleMode.plist")));
	CHECK(init_said("not written"), "and init says it did not write it: %s", said);
	unlink(in_root(PREFS));
	mkdir(in_root(PREFS), 0755);
}

/* A line of more words than argv holds: its "&" is not lost (the job held up the list), it is not run at all. */
static void
scenario_long_line(void)
{
	char line[2048];
	size_t n = 0;

	for (int words = 62; words <= 64; words++) {
		n = (size_t)snprintf(line, sizeof(line), "/bin/job");
		for (int i = 1; i < words; i++) {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " w%d", i);
		}
		snprintf(line + n, sizeof(line) - n, " &");
		forget_calls();
		run_command(line);
		if (words == 62) {
			CHECK(forks == 1 && waits_blocking == 0, "62 words and \"&\": run in the background: %d started, %d waited for",
			    forks, waits_blocking);
		} else {
			CHECK(waits_blocking == 0, "%d words and \"&\": the list does not wait for it (%d waits): %s", words, waits_blocking,
			    said);
		}
	}
	CHECK(forks == 0 && init_said("not run"), "64 words: refused, and said so: %s", said);

	/* 63 words without "&": run whole, its last word a word of its own */
	n = (size_t)snprintf(line, sizeof(line), "/bin/job");
	for (int i = 1; i < 63; i++) {
		n += (size_t)snprintf(line + n, sizeof(line) - n, " w%d", i);
	}
	forget_calls();
	run_command(line);
	CHECK(forks == 1 && waits_blocking == 1 && init_said(" w61 w62\n"), "63 words: run and waited for: %s", said);
}

/* A job list longer than its buffer: what is run ends with a whole line, not the half line the buffer ends in. */
static void
scenario_long_list(void)
{
	static char text[70000];
	const size_t cut = 65536 - 1;   /* what run_rc's buffer holds */
	const char *half = "/bin/cut_in_two_by_the_buffer\n";
	size_t n = 0;

	n += (size_t)snprintf(text, sizeof(text), "/bin/whole\n");
	while (n < cut - 10) {
		size_t room = cut - 10 - n, line = room > 100 ? 100 : room;

		if (room > 100 && room - 100 < 2) {
			line = 50;              /* (no comment line of one byte, a lone newline: it would not be one) */
		}
		memset(text + n, '#', line - 1);
		text[n + line - 1] = '\n';
		n += line;
	}
	memcpy(text + n, half, strlen(half));
	n += strlen(half);
	n += (size_t)snprintf(text + n, sizeof(text) - n, "/bin/after\n");
	put_file(in_root("/rc"), text);

	forget_calls();
	run_rc(in_root("/rc"));
	CHECK(init_said("$ /bin/whole\n"), "the whole line before is run: %.200s", said);
	CHECK(!init_said("$ /bin/cut"), "the half line is not run: %.400s", strstr(said, "$ /bin/cut") ? strstr(said, "$ /bin/cut") : "");
	CHECK(forks == 1, "one command run, not %d", forks);
	CHECK(init_said("does not fit"), "and the list is said not to fit");
}

/* A directory record of length 0 ends the walk; it does not go round for ever. One walk a child, under alarm(). */
#define WALK_NEVER_ENDED 3

static void
on_alarm(int number)
{
	(void)number;
	_exit(WALK_NEVER_ENDED);
}

static void
scenario_zero_record(void)
{
	static const char *const walks[] = { "count_entries", "own_tree", "copy_tree", "graft_tree" };

	mkdir(in_root("/private/var/walk"), 0755);
	for (unsigned i = 0; i < sizeof(walks) / sizeof(walks[0]); i++) {
		int status = -1, pid = fork();

		if (pid == 0) {
			int files = 0, made = 0;

			signal(HOST_SIGALRM, on_alarm);
			alarm(10);
			zero_record_armed = 1;
			switch (i) {
			case 0:
				count_entries("/private/var/walk");
				break;
			case 1:
				own_tree("/private/var/walk", 501);
				break;
			case 2:
				copy_tree("/private/var/walk", "/private/var/walk-copy", &files);
				break;
			default:
				graft_tree("/private/var/walk", "/private/var/walk-graft", 1, &made, &files);
				break;
			}
			_exit(0);
		}
		CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0, "%s ends on a record of length 0: %s", walks[i],
		    WEXITSTATUS(status) == WALK_NEVER_ENDED ? "it went on until alarm() stopped it" : "it did not run");
	}
}

/* Numbers from a boot argument or a job line with more digits than they can be. */
static void
scenario_numbers(void)
{
	char line[64];

	snprintf(line, sizeof(line), "sleep 2");
	forget_calls();
	run_command(line);
	CHECK(sleeps == 10, "sleep 2: 10 pauses of 200 ms, not %d", sleeps);
	snprintf(line, sizeof(line), "sleep 4294967297");
	forget_calls();
	run_command(line);
	CHECK(sleeps == A_DAY * 5, "sleep 4294967297: a day, not %d pauses", sleeps);
	snprintf(line, sizeof(line), "sleep 99999999999999999999999");
	forget_calls();
	run_command(line);
	CHECK(sleeps == A_DAY * 5, "sleep 99999999999999999999999: a day, not %d pauses", sleeps);

	/* the wall clock: 2^64 + 1700000000 is no time, whatever it wraps to */
	boot_line = "rd=md0 epoch=18446744075409551616";
	forget_calls();
	mount_writable();
	CHECK(clock_sets == 0, "epoch=18446744075409551616 sets no clock (it was set to %ld)", clock_set_to);
	boot_line = "rd=md0 epoch=1700000000";
	forget_calls();
	mount_writable();
	CHECK(clock_sets == 1 && clock_set_to == 1700000000, "epoch=1700000000 sets the clock: %d, to %ld", clock_sets, clock_set_to);
}

/* The kernel's build date read back from its version string, as the kernel itself reads it (drivers/iokit/build_date.h). */
static void
scenario_build_date(void)
{
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Wed Nov 11 12:30:45 UTC 2026; root:xnu-12377/DEVELOPMENT_ARM64")
	    == 1794400245L, "a date in November");
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Sun Nov  8 09:05:07 UTC 2026; root:xnu") == 1794128707L,
	    "a day of one digit, padded with a space as date(1) pads it");
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Thu Feb 29 00:00:00 GMT 2024; root:xnu") == 1709164800L,
	    "the 29th of February of a leap year, in GMT");
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Thu Dec 31 23:59:59 UTC 2099") == 4102444799L,
	    "the last second taken, the string ending at the year");
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Wed Nov 11 12:30:45 CET 2026; root:xnu") == 1794400245L - 86400,
	    "a zone other than UTC: a day earlier, never later than the build");
	CHECK(build_date_seconds("Darwin Kernel Version 26.0.0: Mi 11. Nov 12:30:45 UTC 2026; root:xnu") == 0 &&
	    build_date_seconds("Darwin Kernel Version 26.0.0: Wed Nov 11 12:30 UTC 2026; root:xnu") == 0 &&
	    build_date_seconds("Darwin Kernel Version 26.0.0: Wed Nov 11 12:30:45 UTC 1999; root:xnu") == 0 &&
	    build_date_seconds("Darwin Kernel Version 26.0.0: Wed Nov 31") == 0 &&
	    build_date_seconds("Darwin Kernel Version 26.0.0") == 0 && build_date_seconds("") == 0 && build_date_seconds(NULL) == 0,
	    "a date in another language, one cut short, a year before 2000 and no date at all: none");
	CHECK(build_date_floor("") == BUILD_DATE_EARLIEST && build_date_floor("x: Wed Nov 11 12:30:45 UTC 2026") == BUILD_DATE_EARLIEST,
	    "the floor without a date, or with one before this was written, is when this was written");
	CHECK(build_date_floor("x: Mon Jan  7 00:00:00 UTC 2030; root") == 1893974400L, "a later build is the floor");
}

#define KERNEL_2030     "Darwin Kernel Version 27.0.0: Mon Jan  7 00:00:00 UTC 2030; root:xnu-12377/DEVELOPMENT_ARM64"
#define BUILT_2030      1893974400L
#define A_YEAR          (365L * 86400)

/* the time of PATH (one of the test's) set to SECONDS */
static void
set_time(const char *path, long seconds)
{
	struct timespec times[2] = { { seconds, 0 }, { seconds, 0 } };

	CHECK(utimensat(HOST_AT_FDCWD, path, times, 0) == 0, "%s given the time %ld", path, seconds);
}

/*
 * The wall clock raised to what the volumes last knew: a clock behind the newest stamp is raised to it, one ahead of
 * every stamp is left, and a stamp more than ten years past the kernel's build is not taken.
 */
static void
scenario_clock(void)
{
	static const char *const dirs[] = { "/private", "/private/var", "/private/var/db", "/private/var/preferences",
		                            "/private/var/mobile", "/private/var/mobile/Library", PREFS };
	long stamps[3] = { 100, BUILT_2030 + A_YEAR, BUILT_2030 + 30 * A_YEAR };
	unsigned which = 99;

	/* the choice alone */
	CHECK(clock_raise_to(1000, BUILT_2030, stamps, 3, &which) == BUILT_2030 + A_YEAR && which == 1,
	    "a clock in 1970 raised to the newest stamp that is not past ten years after the build (%u)", which);
	which = 99;
	CHECK(clock_raise_to(BUILT_2030 + 2 * A_YEAR, BUILT_2030, stamps, 3, &which) == 0 && which == 99,
	    "a clock ahead of every stamp it may take is left");
	CHECK(clock_raise_to(BUILT_2030, BUILT_2030, stamps, 1, &which) == 0, "nor is it put back to an older one");
	stamps[2] = BUILT_2030 + CLOCK_STAMP_SPAN;
	CHECK(clock_raise_to(1000, BUILT_2030, stamps, 3, &which) == BUILT_2030 + CLOCK_STAMP_SPAN && which == 2,
	    "ten years after the build to the second is still taken");
	stamps[2]++;
	CHECK(clock_raise_to(1000, BUILT_2030, stamps, 3, &which) == BUILT_2030 + A_YEAR, "a second more is not");
	CHECK(clock_raise_to(1000, BUILT_2030, stamps, 0, &which) == 0, "no stamps: nothing");

	/* and process 1 doing it, over directories of the test's (and the host's own root, older than any of these) */
	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		mkdir(in_root(dirs[i]), 0755);
	}
	put_file(in_root("/image/etc/rc"), "exec /sbin/launchd\n");
	set_time(in_root("/private/var"), BUILT_2030 + 2 * A_YEAR);         /* when the store was saved */
	set_time(in_root("/private/var/db"), BUILT_2030 + A_YEAR);
	set_time(in_root("/private/var/preferences"), 100);
	set_time(in_root(PREFS), BUILT_2030 + 3 * A_YEAR);                  /* the newest */
	set_time(in_root("/image/etc/rc"), BUILT_2030 - A_YEAR);
	kernel_version = KERNEL_2030;

	forget_calls();
	clock_now = BUILT_2030;         /* (as the kernel starts it, without a time given) */
	clock_floor();
	CHECK(clock_sets == 1 && clock_set_to == BUILT_2030 + 3 * A_YEAR, "a clock at the build raised to the newest stamp: %d, %ld",
	    clock_sets, clock_set_to);
	CHECK(init_said(PREFS " was written at"), "and said where it came from");

	forget_calls();
	clock_now = BUILT_2030 + 4 * A_YEAR;
	clock_floor();
	CHECK(clock_sets == 0 && init_said("is not behind"), "a clock ahead of every stamp is left: %d", clock_sets);

	/* a directory written by a clock forty years ahead: left out, and the next newest taken */
	set_time(in_root(PREFS), BUILT_2030 + 40 * A_YEAR);
	forget_calls();
	clock_now = 1000;
	clock_floor();
	CHECK(clock_sets == 1 && clock_set_to == BUILT_2030 + 2 * A_YEAR, "the stamp past ten years left out, /private/var taken: %ld",
	    clock_set_to);
	CHECK(init_said("more than ten years past"), "and said so");

	/* with no kernel version to read, the floor is when the header was written; a stamp past ten years after that is not taken */
	kernel_version = NULL;
	set_time(in_root("/private/var"), BUILD_DATE_EARLIEST + 11 * A_YEAR);
	set_time(in_root("/private/var/db"), BUILD_DATE_EARLIEST + A_YEAR);
	set_time(in_root("/image/etc/rc"), 100);
	forget_calls();
	clock_now = 1000;
	clock_floor();
	CHECK(clock_sets == 1 && clock_set_to == BUILD_DATE_EARLIEST + A_YEAR, "without kern.version: %ld", clock_set_to);
	clock_now = 0;
}

/*
 * dasd's budgets: the plists are made and read back by Python's plistlib, a reader and writer that are not ours. KIND
 * names the dictionary: what is in it besides lastModulation, and what lastModulation is.
 */
#define DASD_PLIST      PREFS "/com.apple.dasd.widgetRefreshBudgets.plist"
static const char bplist_py[] =
    "import datetime, plistlib, sys\n"
    "def content(kind):\n"
    "    d = {'budgets': {'widget%d' % i: {'balance': i * 7, 'allocated': float(i)} for i in range(19)}, 'version': 3}\n"
    "    if kind.startswith('keys'):\n"
    "        d = {'k%02d' % i: i for i in range(int(kind[4:]) - 1)}\n"
    "    if kind == 'big':\n"
    "        d['budgets'] = {'widget%d' % i: {'balance': i, 'name': 'w%d' % i} for i in range(300)}\n"
    "    when = {'new': datetime.datetime(2026, 11, 13, 22, 33), 'y1999': datetime.datetime(1999, 12, 31, 23, 59, 59),\n"
    "            'string': '1970-01-01'}.get(kind, datetime.datetime(1970, 1, 1, 0, 0, 34))\n"
    "    if kind != 'none':\n"
    "        d['lastModulation'] = when\n"
    "    return d\n"
    "op, path, kind = sys.argv[1:4]\n"
    "if op == 'make':\n"
    "    fmt = plistlib.FMT_XML if kind == 'xml' else plistlib.FMT_BINARY\n"
    "    open(path, 'wb').write(plistlib.dumps(content(kind), fmt=fmt))\n"
    "else:\n"
    "    want = content(kind)\n"
    "    want.pop('lastModulation', None)\n"
    "    got = plistlib.load(open(path, 'rb'))\n"
    "    sys.exit(0 if got == want else 1)\n";

/* the bytes of PATH (one of the test's), up to SIZE: how many, -1 when it cannot be read */
static ssize_t
file_bytes(const char *path, unsigned char *buf, size_t size)
{
	int fd = open(path, O_RDONLY, 0);
	ssize_t got = 0, n;

	if (fd < 0) {
		return -1;
	}
	while ((size_t)got < size && (n = read(fd, buf + got, size - (size_t)got)) > 0) {
		got += n;
	}
	close(fd);
	return got;
}

static void
put_bytes(const char *path, const unsigned char *data, size_t len)
{
	int fd = open(path, O_WRONLY | HOST_O_CREAT | HOST_O_TRUNC, 0600);

	CHECK(fd >= 0 && write(fd, data, len) == (ssize_t)len, "%s written", path);
	close(fd);
}

/* the plist of KIND made at PATH (one of the test's), or read back from there as KIND without lastModulation */
static int
bplist(const char *op, const char *path, const char *kind)
{
	char command[4600];

	snprintf(command, sizeof(command), "python3 -I '%s' %s '%s' %s", in_root("/bplist.py"), op, path, kind);
	return system(command) == 0;
}

/*
 * A 1970 lastModulation is taken out of dasd's budgets after the clock is raised, and nothing else in the file changes;
 * a later one, another kind of value, an XML plist and a link are left as they are.
 */
static void
scenario_dasd(void)
{
	static const char *const dirs[] = { "/private", "/private/var", "/private/var/mobile", "/private/var/mobile/Library",
		                            PREFS };
	static const char *const taken[] = { "old", "y1999", "keys16", "keys15", "keys2", "big" };
	static const char *const left[] = { "new", "string", "xml", "none" };
	static unsigned char before[65536], after[65536];
	char path[4096], elsewhere[4096];       /* (in_root()'s own buffers are taken again four calls later) */
	ssize_t length;
	struct host_statx x;

	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		mkdir(in_root(dirs[i]), 0755);
	}
	put_file(in_root("/bplist.py"), bplist_py);
	snprintf(path, sizeof(path), "%s", in_root(DASD_PLIST));
	snprintf(elsewhere, sizeof(elsewhere), "%s", in_root("/private/elsewhere.plist"));
	forget_calls();
	forget_old_modulation();
	CHECK(said[0] == 0, "no budgets: nothing done, nothing said: %s", said);

	for (unsigned i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
		CHECK(bplist("make", path, taken[i]), "%s made", taken[i]);
		chmod(path, 0640);
		if (getuid() == 0) {
			chown(path, 501, 20);
		}
		forget_calls();
		forget_old_modulation();
		CHECK(bplist("check", path, taken[i]), "%s: lastModulation gone, the rest as it was: %s", taken[i], said);
		CHECK(init_said("lastModulation of ") && init_said("taken out"), "%s: and said so: %s", taken[i], said);
		CHECK(host_statx(path, 0, &x) == 0 && (x.mode & 07777) == 0640 && x.nlink == 1, "%s: its mode kept: %o",
		    taken[i], x.mode & 07777);
		if (getuid() == 0) {
			CHECK(x.uid == 501 && x.gid == 20, "%s: and its owner: %u:%u", taken[i], x.uid, x.gid);
		}
		if (i == 0) {
			CHECK(init_said("lastModulation of 34 (before 2001)"), "the time said is the one stored: %s", said);
		}
		/* and a second time there is nothing more to do */
		forget_calls();
		forget_old_modulation();
		CHECK(said[0] == 0 && bplist("check", path, taken[i]), "%s: run again, nothing changed: %s", taken[i], said);
	}

	for (unsigned i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
		CHECK(bplist("make", path, left[i]), "%s made", left[i]);
		length = file_bytes(path, before, sizeof(before));
		CHECK(length > 0 && host_statx(path, 0, &x) == 0, "%s looked at", left[i]);
		{
			uint64_t ino = x.ino;

			forget_calls();
			forget_old_modulation();
			CHECK(host_statx(path, 0, &x) == 0 && x.ino == ino &&
			    file_bytes(path, after, sizeof(after)) == length && memcmp(before, after, (size_t)length) == 0,
			    "%s: the file is left as it was: %s", left[i], said);
		}
		CHECK(!init_said("taken out"), "%s: nothing said to be taken out: %s", left[i], said);
		if (strcmp(left[i], "string") == 0) {
			CHECK(init_said("is not a date"), "a value that is not a date: said: %s", said);
		}
	}

	/* a link in the file's place: not followed, the file it leads to not touched */
	CHECK(bplist("make", elsewhere, "old"), "the file a link leads to made");
	unlink(path);
	CHECK(symlink(elsewhere, path) == 0, "the link planted");
	forget_calls();
	forget_old_modulation();
	CHECK(init_said("not a plain file"), "a link: left alone, and said so: %s", said);
	CHECK(!bplist("check", elsewhere, "old"), "and the file behind it still has its 1970 date");

	/* what is no plist at all, or one cut short anywhere: refused, nothing written */
	unlink(path);
	put_file(path, "bplist00 and then nothing that a plist has");
	forget_calls();
	forget_old_modulation();
	CHECK(init_said("not a binary plist this reads") &&
	    strcmp(file_text(path), "bplist00 and then nothing that a plist has") == 0, "a file that is no plist: left: %s", said);
	CHECK(bplist("make", path, "old"), "the whole plist made");
	length = file_bytes(path, before, sizeof(before));
	for (ssize_t cut = 1; cut < length; cut++) {
		put_bytes(path, before, (size_t)cut);
		forget_calls();
		forget_old_modulation();
		CHECK(file_bytes(path, after, sizeof(after)) == cut && memcmp(before, after, (size_t)cut) == 0 &&
		    !init_said("taken out"), "cut to %zd bytes: left as it is: %s", cut, said);
	}
	/* and every byte of the trailer changed in turn: whatever it then says, nothing is read or written past the file */
	for (ssize_t at = length - 32; at < length; at++) {
		for (unsigned v = 0; v < 256; v += 51) {
			memcpy(after, before, (size_t)length);
			after[at] = (unsigned char)v;
			put_bytes(path, after, (size_t)length);
			forget_calls();
			forget_old_modulation();
		}
	}
	CHECK(1, "a trailer that says anything: no fault");
	unlink(path);
	unlink(elsewhere);
	unlink(in_root("/bplist.py"));
}

#define RTC_FILE        "/private/var/db/rtc-offset"

/*
 * The wall clock kept across boots by the PMIC's clock: the host's time less the clock's count saved when epoch= gave
 * it, and at a boot without the host the clock raised to the count plus what was saved -- never put back, and not
 * taken more than ten years past the kernel's build.
 */
static void
scenario_rtc_saved(void)
{
	mkdir(in_root("/private"), 0755);
	mkdir(in_root("/private/var"), 0755);
	mkdir(in_root("/private/var/db"), 0755);
	kernel_version = KERNEL_2030;
	unlink(in_root(RTC_FILE));

	/* a kernel without the clock (or a board without one): said, nothing written, the clock left */
	rtc_answer = -1;
	boot_line = "rd=md0 epoch=1900000000";
	forget_calls();
	mount_writable();
	clock_rtc();
	CHECK(clock_sets == 1 && init_said("no count of the PMIC's clock"), "without hw.rtc_seconds: said, the clock as epoch= set it");
	CHECK(file_text(in_root(RTC_FILE))[0] == 0, "and nothing saved");

	/* the host gives the time: what it is past the count, at the same moment, is saved; the clock is not touched again */
	rtc_answer = 1000;
	forget_calls();
	mount_writable();
	clock_rtc();
	CHECK(strcmp(file_text(in_root(RTC_FILE)), "1899999000\n") == 0, "the host's time less the count saved: \"%s\"",
	    file_text(in_root(RTC_FILE)));
	CHECK(clock_sets == 1 && clock_set_to == 1900000000 && init_said("kept in " RTC_FILE), "the clock set by epoch= alone (%d)",
	    clock_sets);

	/* a link the store put back in the file's place is not written through */
	put_file(in_root("/victim"), "root's\n");
	unlink(in_root(RTC_FILE));
	CHECK(symlink(in_root("/victim"), in_root(RTC_FILE)) == 0, "a link in the file's place");
	rtc_answer = 2000;
	forget_calls();
	mount_writable();
	clock_rtc();
	CHECK(strcmp(file_text(in_root("/victim")), "root's\n") == 0, "the link's target left as it was");
	CHECK(strcmp(file_text(in_root(RTC_FILE)), "1899998000\n") == 0, "the file made anew: \"%s\"", file_text(in_root(RTC_FILE)));
	unlink(in_root("/victim"));
	rtc_answer = -1;
}

static void
scenario_rtc_applied(void)
{
	kernel_version = KERNEL_2030;
	put_file(in_root(RTC_FILE), "1899999000\n");

	/* a boot without the host: the clock (at the build, or raised by the floor) raised to the count plus the saved */
	rtc_answer = 5000;
	boot_line = "rd=md0";
	forget_calls();
	mount_writable();
	clock_now = BUILT_2030;
	clock_rtc();
	CHECK(clock_sets == 1 && clock_set_to == 1900004000, "the clock set to the count and the saved: %d, %ld", clock_sets,
	    clock_set_to);
	CHECK(init_said("set to that: ok"), "and said");
	CHECK(strcmp(file_text(in_root(RTC_FILE)), "1899999000\n") == 0, "and the file not written without the host's time");

	/* a clock already past it (the floor's, a firmware's) is not put back: a count that started again from 0 */
	forget_calls();
	clock_now = 1900010000;
	clock_rtc();
	CHECK(clock_sets == 0 && init_said("is not behind it"), "a clock ahead of the count and the saved is left: %d", clock_sets);

	/* a time past ten years after the kernel's build is taken for nonsense */
	put_file(in_root(RTC_FILE), "2300000000\n");
	forget_calls();
	clock_now = BUILT_2030;
	clock_rtc();
	CHECK(clock_sets == 0 && init_said("more than ten years past the kernel's build"), "an absurd time left out: %d, %ld",
	    clock_sets, clock_set_to);
	/* ... and ten years to the second is not */
	put_file(in_root(RTC_FILE), "2209588600\n");          /* (BUILT_2030 + CLOCK_STAMP_SPAN - 5000) */
	forget_calls();
	clock_rtc();
	CHECK(clock_sets == 1 && clock_set_to == BUILT_2030 + CLOCK_STAMP_SPAN, "ten years after the build to the second: %ld",
	    clock_set_to);

	/* a count ahead of the time of day: what is saved is less than 0 */
	put_file(in_root(RTC_FILE), "-1000\n");
	rtc_answer = 1900005000;
	forget_calls();
	clock_rtc();
	CHECK(clock_sets == 1 && clock_set_to == 1900004000, "a saved difference below 0: %ld", clock_set_to);

	/* a file that is not a number, and none at all: said, the clock left */
	put_file(in_root(RTC_FILE), "soon\n");
	forget_calls();
	clock_rtc();
	CHECK(clock_sets == 0 && init_said("no boot with the host has said"), "a file of words: %d", clock_sets);
	unlink(in_root(RTC_FILE));
	forget_calls();
	clock_rtc();
	CHECK(clock_sets == 0 && init_said("no boot with the host has said"), "no file: %d", clock_sets);
	rtc_answer = -1;
	clock_now = 0;
}

/* An exec that fails gives process 1 its standard error back; ">>" on an exec line is refused. */
static void
scenario_exec(void)
{
	struct host_statx before, after;
	int kept = dup(2), err = open(in_root("/stderr"), O_WRONLY | HOST_O_CREAT | HOST_O_TRUNC, 0600);
	char line[128];

	CHECK(kept >= 0 && err >= 0 && dup2(err, 2) == 2, "standard error on a file of the test's");
	close(err);
	put_file(in_root("/image/etc/quiet"), "");
	CHECK(statx(2, "", HOST_AT_EMPTY_PATH, HOST_STATX_BASIC, &before) == 0, "standard error looked at");

	snprintf(line, sizeof(line), "exec /bin/not_there");
	forget_calls();
	run_command(line);
	CHECK(execs == 1 && exec_stderr_was_null, "with etc/quiet, the program is started with standard error on /dev/null");
	CHECK(statx(2, "", HOST_AT_EMPTY_PATH, HOST_STATX_BASIC, &after) == 0 && after.ino == before.ino &&
	    after.dev_minor == before.dev_minor && after.dev_major == before.dev_major,
	    "when it cannot be, init's own standard error is back where it was");

	snprintf(line, sizeof(line), "exec /bin/launchd >> /private/var/log/out");
	forget_calls();
	run_command(line);
	CHECK(execs == 0 && init_said("not run"), "\">>\" on an exec line: not run, and said so (%d execs)", execs);

	dup2(kept, 2);
	close(kept);
	unlink(in_root("/image/etc/quiet"));
}

int
main(void)
{
	static char image[600];
	const char *tmp = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/init_test.%d", tmp != NULL && *tmp != 0 ? tmp : "/tmp", (int)getpid());
	if (mkdir(root, 0755) != 0) {
		printf("init_test: %s cannot be made\n", root);
		return 1;
	}
	CHECK(host_statx("/dev/null", 1, &dev_null) == 0, "/dev/null looked at");
	snprintf(image, sizeof(image), "%s/image", root);
	mkdir(image, 0755);
	mkdir(in_root("/image/etc"), 0755);
	base = image;

	scenario_preferences();
	scenario_long_line();
	scenario_long_list();
	scenario_numbers();
	scenario_build_date();
	scenario_clock();
	scenario_dasd();
	scenario_rtc_saved();
	scenario_rtc_applied();
	scenario_exec();
	scenario_zero_record();

	{
		char command[700];

		snprintf(command, sizeof(command), "rm -rf '%s'", root);
		system(command);
	}
	printf("init_test: %d checks, %d failed\n", t_checks, t_failures);
	return t_failures != 0;
}
