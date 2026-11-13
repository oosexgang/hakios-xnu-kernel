/*
 * The first program of the root tools/qemu-root.sh makes: what the kernel starts as /sbin/launchd when
 * it has no Darwin to start. It shows that the kernel runs a program of its own user space -- exec, the
 * system calls, devfs, the console -- and says what the kernel says of itself: its version, the board
 * the loader named, the processors and the memory. Then it is a small shell on the console (the serial
 * port is its keyboard in QEMU): `help` lists what it can do.
 *
 * Nothing of Apple's: no libSystem, no dyld. A static arm64 executable that makes its system calls
 * itself (svc 0x80, the number in x16, the carry flag set on an error), built and linked by
 * tools/qemu-root.sh with clang and ld64.lld.
 */
#include <stddef.h>
#include <stdint.h>

/* bsd/kern/syscalls.master */
#define SYS_exit                1
#define SYS_read                3
#define SYS_write               4
#define SYS_open                5
#define SYS_close               6
#define SYS_getpid              20
#define SYS_sigsuspend          111
#define SYS_sysctlbyname        274
#define O_RDWR                  2
#define O_NOCTTY                0x20000

static long
syscall6(long number, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	register long x16 __asm__("x16") = number;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs"
	    : "+r"(x0), "+r"(x1), [failed] "=r"(failed)
	    : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
	    : "memory", "cc");
	return failed ? -x0 : x0;
}

/* ---- text --------------------------------------------------------------------------------------- */

static int console = 1;

static size_t
length(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0') {
		n++;
	}
	return n;
}

static int
same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void
say(const char *s)
{
	syscall6(SYS_write, console, (long)s, (long)length(s), 0, 0, 0);
}

static void
say_number(uint64_t n)
{
	char digits[24];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	say(&digits[i]);
}

/* ---- what the kernel says of itself ------------------------------------------------------------- */

/* a sysctl by its name: a string, or a number of 4 or 8 bytes; 0 when the kernel has no such name */
static int
sysctl_string(const char *name, char *out, size_t size)
{
	size_t got = size - 1;

	if (syscall6(SYS_sysctlbyname, (long)name, (long)length(name), (long)out, (long)&got, 0, 0) < 0) {
		return 0;
	}
	out[got < size ? got : size - 1] = '\0';
	return 1;
}

static int
sysctl_number(const char *name, uint64_t *value)
{
	union { uint32_t u32; uint64_t u64; } got = { 0 };
	size_t size = sizeof(got);

	if (syscall6(SYS_sysctlbyname, (long)name, (long)length(name), (long)&got, (long)&size, 0, 0) < 0) {
		return 0;
	}
	*value = size == 4 ? got.u32 : got.u64;
	return 1;
}

static void
line(const char *what, const char *name)
{
	char text[256];

	if (sysctl_string(name, text, sizeof(text))) {
		say(what);
		say(text);
		say("\n");
	}
}

static void
about(void)
{
	char text[256];
	uint64_t n;

	line("  kernel     ", "kern.version");
	line("  release    ", "kern.osrelease");
	if (sysctl_string("hw.board.name", text, sizeof(text))) {
		say("  board      ");
		say(text);
		if (sysctl_string("hw.board.description", text, sizeof(text))) {
			say(" (");
			say(text);
			say(")");
		}
		say("\n");
	}
	if (sysctl_number("hw.ncpu", &n)) {
		say("  processors ");
		say_number(n);
		say("\n");
	}
	if (sysctl_number("hw.memsize", &n)) {
		say("  memory     ");
		say_number(n >> 20);
		say(" MB\n");
	}
	line("  machine    ", "hw.machine");
}

/* ---- the shell ---------------------------------------------------------------------------------- */

#define WORDS   16

/* the line split at spaces, in place; how many words */
static int
split(char *text, char **words)
{
	int n = 0;

	while (*text != '\0' && n < WORDS) {
		while (*text == ' ' || *text == '\t') {
			*text++ = '\0';
		}
		if (*text == '\0') {
			break;
		}
		words[n++] = text;
		while (*text != '\0' && *text != ' ' && *text != '\t') {
			text++;
		}
	}
	return n;
}

/*
 * A value as the kernel types it: its name to its numbers (sysctl 0.3, name2oid), their kind and format
 * (0.4, oidfmt: CTLTYPE_INT, _STRING, _QUAD, and a format that says whether the number is signed), then
 * the value. What /usr/sbin/sysctl does, without it.
 */
#define SYS___sysctl            202
#define CTLTYPE_MASK            0xf
#define CTLTYPE_NODE            1
#define CTLTYPE_INT             2
#define CTLTYPE_STRING          3
#define CTLTYPE_QUAD            4
#define CTL_MAXNAME             12

static void
say_signed(int64_t n)
{
	if (n < 0) {
		say("-");
		say_number((uint64_t)-n);
	} else {
		say_number((uint64_t)n);
	}
}

static void
command_sysctl(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		int query[2] = { 0, 3 }, mib[CTL_MAXNAME + 2] = { 0, 4 };
		size_t size = CTL_MAXNAME * sizeof(int);
		union { uint32_t kind; char bytes[64]; } format;
		union { char text[512]; int32_t i32[128]; int64_t i64[64]; } value;

		say(argv[i]);
		say(": ");
		if (syscall6(SYS___sysctl, (long)query, 2, (long)&mib[2], (long)&size, (long)argv[i], (long)length(argv[i])) < 0) {
			say("(no such name)\n");
			continue;
		}
		unsigned depth = (unsigned)(size / sizeof(int));
		size = sizeof(format);
		if (syscall6(SYS___sysctl, (long)mib, depth + 2, (long)&format, (long)&size, 0, 0) < 0) {
			say("(no format)\n");
			continue;
		}
		const char *fmt = format.bytes + sizeof(uint32_t);
		int type = (int)(format.kind & CTLTYPE_MASK), is_unsigned = fmt[1] == 'U';
		if (type == CTLTYPE_NODE) {
			say("(a node: its children have the values)\n");
			continue;
		}
		size = sizeof(value) - 1;
		if (syscall6(SYS___sysctl, (long)&mib[2], depth, (long)&value, (long)&size, 0, 0) < 0) {
			say("(the kernel will not say)\n");
			continue;
		}
		if (type == CTLTYPE_STRING) {
			value.text[size] = '\0';
			say(size > 1 ? value.text : "(empty)");
		} else if (type == CTLTYPE_INT && size >= 4) {
			for (size_t n = 0; n < size / 4; n++) {
				say(n ? " " : "");
				is_unsigned ? say_number((uint32_t)value.i32[n]) : say_signed(value.i32[n]);
			}
		} else if (type == CTLTYPE_QUAD && size >= 8) {
			for (size_t n = 0; n < size / 8; n++) {
				say(n ? " " : "");
				is_unsigned ? say_number((uint64_t)value.i64[n]) : say_signed(value.i64[n]);
			}
		} else {
			say("(");
			say_number(size);
			say(" bytes of ");
			say(fmt);
			say(")");
		}
		say("\n");
	}
}

static void
command_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		say(argv[i]);
		say(i + 1 < argc ? " " : "");
	}
	say("\n");
}

/* ---- files ---- */

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
say_padded(uint64_t n, int width)
{
	char digits[24];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	for (int pad = width - ((int)sizeof(digits) - 1 - i); pad > 0; pad--) {
		say(" ");
	}
	say(&digits[i]);
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

static void
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

static void
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

/* ---- processes, time ---- */

#define SYS_gettimeofday        116
#define SYS_proc_info           336
#define PROC_INFO_CALL_LISTPIDS 1
#define PROC_INFO_CALL_PIDINFO  2
#define PROC_ALL_PIDS           1
#define PROC_PIDTBSDINFO        3

struct proc_bsdinfo {                   /* sys/proc_info.h */
	uint32_t        flags, status, xstatus, pid, ppid, uid, gid, ruid, rgid, svuid, svgid, rfu_1;
	char            comm[16], name[32];
	uint32_t        nfiles, pgid, pjobc, e_tdev, e_tpgid;
	int32_t         nice;
	uint64_t        start_tvsec, start_tvusec;
};

static void
command_ps(int argc, char **argv)
{
	int32_t pids[256];
	(void)argc;
	(void)argv;

	long bytes = syscall6(SYS_proc_info, PROC_INFO_CALL_LISTPIDS, PROC_ALL_PIDS, 0, 0, (long)pids, sizeof(pids));
	if (bytes < 0) {
		say("ps: the kernel will not list its processes\n");
		return;
	}
	say("  PID  PPID  UID  NAME\n");
	for (long i = 0; i < bytes / (long)sizeof(pids[0]); i++) {
		struct proc_bsdinfo info;

		if (syscall6(SYS_proc_info, PROC_INFO_CALL_PIDINFO, pids[i], PROC_PIDTBSDINFO, 0, (long)&info, sizeof(info)) !=
		    (long)sizeof(info)) {
			continue;
		}
		say_padded(info.pid, 5);
		say_padded(info.ppid, 6);
		say_padded(info.uid, 5);
		say("  ");
		say(info.name[0] != '\0' ? info.name : info.comm);
		say("\n");
	}
}

static void
command_uptime(int argc, char **argv)
{
	int64_t booted[2] = { 0, 0 }, now[2] = { 0, 0 };
	size_t size = sizeof(booted);
	(void)argc;
	(void)argv;

	if (syscall6(SYS_sysctlbyname, (long)"kern.boottime", 13, (long)booted, (long)&size, 0, 0) < 0) {
		say("uptime: the kernel will not say when it started\n");
		return;
	}
	if (syscall6(SYS_gettimeofday, (long)now, 0, 0, 0, 0, 0) != 0) {
		say("uptime: the kernel will not say what time it is\n");
		return;
	}
	uint64_t up = (uint64_t)(now[0] - booted[0]);
	say("up ");
	say_number(up / 60);
	say(" min ");
	say_number(up % 60);
	say(" s, since ");
	say_number((uint64_t)booted[0]);
	say(" (seconds since 1970)\n");
}

static void command_help(int argc, char **argv);

static void
command_about(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	about();
}

static const struct {
	const char *name, *what;
	void (*run)(int argc, char **argv);
} commands[] = {
	{ "help", "what this shell can do", command_help },
	{ "about", "what the kernel says of itself", command_about },
	{ "sysctl", "NAME... the kernel's value of each", command_sysctl },
	{ "echo", "WORDS... say them", command_echo },
	{ "ls", "[PATH...] what is in a directory", command_ls },
	{ "cat", "FILE... what is in a file", command_cat },
	{ "ps", "the processes", command_ps },
	{ "uptime", "how long the kernel has run", command_uptime },
};

static void
command_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		say("  ");
		say(commands[i].name);
		say(length(commands[i].name) < 8 ? "\t\t" : "\t");
		say(commands[i].what);
		say("\n");
	}
}

static void
run(char *text)
{
	char *argv[WORDS];
	int argc = split(text, argv);

	if (argc == 0) {
		return;
	}
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (same(argv[0], commands[i].name)) {
			commands[i].run(argc, argv);
			return;
		}
	}
	say(argv[0]);
	say(": not a command here (help lists them)\n");
}

void
start(void)
{
	char text[256];
	long fd = syscall6(SYS_open, (long)"/dev/console", O_RDWR | O_NOCTTY, 0, 0, 0, 0);

	if (fd >= 0) {
		console = (int)fd;
	}
	say("\n");
	say("================================================================\n");
	say("  user space: /sbin/launchd, pid ");
	say_number((uint64_t)syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0));
	say(", a program of this repository\n");
	say("================================================================\n");
	about();
	say("  the kernel ran exec, devfs and the console for it: all is well.\n");
	say("================================================================\n");
	if (fd < 0) {
		for (;;) {
			syscall6(SYS_sigsuspend, 0, 0, 0, 0, 0, 0);
		}
	}
	say("a shell on the console: help lists what it can do\n");
	for (;;) {
		say("# ");
		long got = syscall6(SYS_read, console, (long)text, sizeof(text) - 1, 0, 0, 0);
		if (got <= 0) {
			continue;
		}
		text[got] = '\0';
		for (long i = 0; i < got; i++) {
			if (text[i] == '\n' || text[i] == '\r') {
				text[i] = '\0';
			}
		}
		run(text);
	}
}
