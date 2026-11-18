/* tools/qemu-init: the first process: what it says of the kernel, and the shell that reads the console */
#include "init.h"

/* bsd/kern/syscalls.master */
#define O_RDWR                  2
#define O_NOCTTY                0x20000

/* a sysctl by its name: a string, or a number of 4 or 8 bytes; 0 when the kernel has no such name */
int
sysctl_string(const char *name, char *out, size_t size)
{
	size_t got = size - 1;

	if (syscall6(SYS_sysctlbyname, (long)name, (long)length(name), (long)out, (long)&got, 0, 0) < 0) {
		return 0;
	}
	out[got < size ? got : size - 1] = '\0';
	return 1;
}

int
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

void
command_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		say(argv[i]);
		say(i + 1 < argc ? " " : "");
	}
	say("\n");
}

#define SYS_chdir               12
#define SYS_fcntl               92
#define F_GETPATH               50
#define O_RDONLY_DIRECTORY      0x100000        /* (O_RDONLY | O_DIRECTORY) */

/* DIR: where relative paths start from (/ without one) */
static void
command_cd(int argc, char **argv)
{
	long error = syscall6(SYS_chdir, (long)(argc > 1 ? argv[1] : "/"), 0, 0, 0, 0, 0);

	if (error != 0) {
		say("cd: ");
		say(argc > 1 ? argv[1] : "/");
		say(": ");
		say_error(error);
		say("\n");
	}
}

/* where that is: the kernel's path of ".", as getcwd() asks it (fcntl F_GETPATH) */
static void
command_pwd(int argc, char **argv)
{
	char path[1024];
	long fd = syscall6(SYS_open, (long)".", O_RDONLY_DIRECTORY, 0, 0, 0, 0);

	(void)argc;
	(void)argv;
	if (fd < 0 || syscall6(SYS_fcntl, fd, F_GETPATH, (long)path, 0, 0, 0) != 0) {
		say("pwd: the kernel has no path for it\n");
	} else {
		say(path);
		say("\n");
	}
	if (fd >= 0) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
}

#define SYS_getrusage           117
#define RUSAGE_SELF             0

static int run_words(int argc, char **argv);

/* the processor time this process has had, in microseconds: its user and system times (getrusage) */
static void
used(uint64_t *user_us, uint64_t *system_us)
{
	struct { int64_t user_s; int32_t user_us, pad1; int64_t system_s; int32_t system_us, pad2; int64_t rest[14]; } usage = { 0 };

	syscall6(SYS_getrusage, RUSAGE_SELF, (long)&usage, 0, 0, 0, 0);
	*user_us = (uint64_t)usage.user_s * 1000000 + (uint64_t)usage.user_us;
	*system_us = (uint64_t)usage.system_s * 1000000 + (uint64_t)usage.system_us;
}

/* COMMAND...: the command, and how long it took: on the clock, and of the processors' time, in user space and in the kernel */
static void
command_time(int argc, char **argv)
{
	uint64_t began = now_us(), user0, system0, user1, system1;
	char line[160];
	size_t at = 0;

	if (argc < 2) {
		say("time COMMAND [ARGUMENTS]\n");
		return;
	}
	used(&user0, &system0);
	if (!run_words(argc - 1, argv + 1)) {
		return;
	}
	used(&user1, &system1);
	at = append(line, at, sizeof(line), "time: ");
	at = append_number(line, at, sizeof(line), (now_us() - began) / 1000);
	at = append(line, at, sizeof(line), " ms; of the processors' time ");
	at = append_number(line, at, sizeof(line), (user1 - user0) / 1000);
	at = append(line, at, sizeof(line), " ms in user space, ");
	at = append_number(line, at, sizeof(line), (system1 - system0) / 1000);
	at = append(line, at, sizeof(line), " ms in the kernel\n");
	syscall6(SYS_write, console, (long)line, (long)at, 0, 0, 0);
}

#define SYS_wait4               7
#define SYS_setuid              23
#define SYS_getuid              24
#define SYS_geteuid             25
#define SYS_getgid              47
#define SYS_setgroups           80
#define SYS_setgid              181

/* UID:GID COMMAND...: the command in a child that has become that user and group (setgroups, setgid, setuid):
 * what the kernel lets a user who is not root do and not do */
static void
command_as(int argc, char **argv)
{
	unsigned uid = 0, gid = 0;
	const char *c = argc > 1 ? argv[1] : "";
	int in_child = 0;
	int32_t status = 0;
	long pid, error;

	for (; *c >= '0' && *c <= '9'; c++) {
		uid = uid * 10 + (unsigned)(*c - '0');
	}
	if (*c == ':') {
		for (c++; *c >= '0' && *c <= '9'; c++) {
			gid = gid * 10 + (unsigned)(*c - '0');
		}
	}
	if (argc < 3 || *c != '\0' || c == argv[1]) {
		say("as UID:GID COMMAND [ARGUMENTS]\n");
		return;
	}
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("as: no child: ");
		say_error(pid);
		say("\n");
		return;
	}
	if (in_child) {
		char line[128];
		size_t at;

		if ((error = syscall6(SYS_setgroups, 1, (long)&gid, 0, 0, 0, 0)) != 0 ||
		    (error = syscall6(SYS_setgid, gid, 0, 0, 0, 0, 0)) != 0 ||
		    (error = syscall6(SYS_setuid, uid, 0, 0, 0, 0, 0)) != 0) {
			say("as: ");
			say(argv[1]);
			say(" cannot be had: ");
			say_error(error);
			say("\n");
			syscall6(SYS_exit, 126, 0, 0, 0, 0, 0);
		}
		at = append(line, 0, sizeof(line), "as: user ");
		at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_getuid, 0, 0, 0, 0, 0, 0));
		at = append(line, at, sizeof(line), " (effective ");
		at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_geteuid, 0, 0, 0, 0, 0, 0));
		at = append(line, at, sizeof(line), "), group ");
		at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_getgid, 0, 0, 0, 0, 0, 0));
		at = append(line, at, sizeof(line), "\n");
		say(line);
		syscall6(SYS_exit, run_words(argc - 2, argv + 2) ? 0 : 127, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
}

#define SYS_setpriority         96
#define SYS_getpriority         100
#define PRIO_PROCESS            0

/* N COMMAND...: the command in a child whose priority is N, -20 (first) to 20 (last) (setpriority); only root may
 * put a process before others (a negative N) */
static void
command_nice(int argc, char **argv)
{
	int negative = argc > 1 && argv[1][0] == '-';
	unsigned n = argc > 1 ? parse_number(argv[1] + negative) : 0;
	int in_child = 0;
	int32_t status = 0;
	long pid, error, now;

	if (argc < 3 || n > 20 || (n == 0 && !same(argv[1] + negative, "0"))) {
		say("nice N COMMAND [ARGUMENTS] (N from -20 to 20)\n");
		return;
	}
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("nice: no child: ");
		say_error(pid);
		say("\n");
		return;
	}
	if (in_child) {
		char line[96];
		size_t at;

		if ((error = syscall6(SYS_setpriority, PRIO_PROCESS, 0, negative ? -(long)n : (long)n, 0, 0, 0)) != 0) {
			say("nice: ");
			say(argv[1]);
			say(" not had: ");
			say_error(error);
			say("\n");
			syscall6(SYS_exit, 126, 0, 0, 0, 0, 0);
		}
		now = syscall6(SYS_getpriority, PRIO_PROCESS, 0, 0, 0, 0, 0);
		at = append(line, 0, sizeof(line), now < 0 ? "nice: priority -" : "nice: priority ");
		at = append_number(line, at, sizeof(line), (uint64_t)(now < 0 ? -now : now));
		at = append(line, at, sizeof(line), "\n");
		say(line);
		syscall6(SYS_exit, run_words(argc - 2, argv + 2) ? 0 : 127, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
}

/* the screen emptied and the cursor at its top: VT100's, which the kernel's video console and a terminal on the
 * serial port both take */
static void
command_clear(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	say("\033[H\033[2J");
}

void
command_about(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	about();
}

/* the commands, by what of the kernel they try: an entry with no name is a heading of help's */
static const struct {
	const char *name, *what;
	void (*run)(int argc, char **argv);
} commands[] = {
	{ NULL, "the shell", NULL },
	{ "help", "what this shell can do", command_help },
	{ "echo", "WORDS... say them", command_echo },
	{ "clear", "the screen emptied", command_clear },
	{ "time", "COMMAND... how long it took, and of the processors' time (getrusage)", command_time },
	{ "as", "UID:GID COMMAND... the command as that user and group, in a child", command_as },
	{ "nice", "N COMMAND... the command in a child of that priority, -20 (first) to 20 (last)", command_nice },
	{ "cd", "[DIR] where relative paths start from", command_cd },
	{ "pwd", "where that is", command_pwd },
	{ NULL, "the kernel", NULL },
	{ "about", "what the kernel says of itself", command_about },
	{ "sysctl", "NAME[=VALUE]... | -a [PREFIX] the kernel's values, set if given; all of them", command_sysctl },
	{ "vm", "the virtual memory's pages and faults (host_statistics64)", command_vm },
	{ "random", "[KB] the kernel's random bytes (getentropy, /dev/random), their values counted", command_random },
	{ "dmesg", "[LINES] the kernel's latest messages", command_dmesg },
	{ "uptime", "how long the kernel has run", command_uptime },
	{ "date", "the date and time, UTC", command_date },
	{ NULL, "files", NULL },
	{ "ls", "[PATH...] what is in a directory (this one, without a PATH)", command_ls },
	{ "cat", "FILE... what is in a file", command_cat },
	{ "hash", "FILE... each read to its end, timed, hashed", command_hash },
	{ "mount", "TYPE DEVICE DIR a disk's file system (read only), or ramfs", command_mount },
	{ "df", "the file systems mounted, their size and room", command_df },
	{ "mkdir", "DIR... new directories", command_mkdir },
	{ "write", "[-a] FILE WORDS... the words as all of a file; -a: added to its end", command_write },
	{ "rm", "PATH... files, or empty directories, gone", command_rm },
	{ "stat", "PATH... kind, permissions, links, inode, size, owner; where a symbolic link points", command_stat },
	{ "ln", "[-s] TARGET NAME another name for a file; -s a symbolic link", command_ln },
	{ "mv", "OLD NEW renamed", command_mv },
	{ "cp", "SOURCE TARGET the file's bytes into a new one of the same permissions", command_cp },
	{ "chmod", "MODE PATH... the permissions (octal)", command_chmod },
	{ "chown", "UID:GID PATH... whose files are", command_chown },
	{ "truncate", "FILE BYTES cut to that size, or grown with zeros", command_truncate },
	{ "touch", "FILE [SECONDS] made if not there; its times set, to now or SECONDS since 1970 (utimes)", command_touch },
	{ "chflags", "FLAGS PATH... a file's flags, in hexadecimal (2: not to be changed, 4: only added to)", command_chflags },
	{ "access", "PATH... whether this process may read, write and run each (access)", command_access },
	{ "umask", "[MODE] what a new file is made without (octal)", command_umask },
	{ "xattr", "FILE [NAME VALUE] | -d FILE NAME extended attributes: listed, set, taken away", command_xattr },
	{ "files", "[LIMIT] files opened until the kernel's limit on descriptors says no", command_files },
	{ "fsize", "FILE BYTES files no larger than BYTES (RLIMIT_FSIZE): writes cut, then refused", command_fsize },
	{ "appends", "FILE COUNT two processes adding to the file at once (O_APPEND): every record whole", command_appends },
	{ "opens", "FILE O_EXCL, O_NOFOLLOW, O_DIRECTORY and a directory to write, as BSD has them", command_opens },
	{ "dir-walk", "DIR COUNT a directory of COUNT files read a little at a time; emptied as it is read", command_dir_walk },
	{ "descriptors", "FILE lseek, pread, pwrite, dup, dup2, O_APPEND, a non-blocking pipe", command_descriptors },
	{ "vectors", "FILE three pieces written in one writev, read back in one readv", command_vectors },
	{ "fill", "FILE BYTES a file of made-up bytes (byte i is i * 7 mod 251)", command_fill },
	{ "mmap", "FILE the file's pages, checked against its reads", command_mmap },
	{ "mmap-write", "FILE the file written through a shared mapping, and read back", command_mmap_write },
	{ "mmap-private", "FILE the file mapped private and written through: the mapping changed, the file not", command_mmap_private },
	{ "mmap-coherent", "FILE written while it is mapped: the mapping has the bytes at once", command_mmap_coherent },
	{ "memory", "MB that much memory of its own, written and read back", command_memory },
	{ NULL, "the network", NULL },
	{ "ifconfig", "[NAME ADDRESS [MASK]] the interfaces, or an address for one", command_ifconfig },
	{ "ping", "ADDRESS [COUNT] ICMP echo: is it there", command_ping },
	{ "serve", "[COUNT] a page over HTTP on port 80 (or /bytes/N: N bytes)", command_serve },
	{ "get", "ADDRESS PORT [PATH] a page asked for over HTTP", command_get },
	{ "udp", "ADDRESS PORT WORDS... a datagram, and its answer", command_udp },
	{ "echo-udp", "PORT [COUNT] datagrams on a UDP port sent back", command_echo_udp },
	{ NULL, "processes", NULL },
	{ "ps", "the processes", command_ps },
	{ "id", "this process's user and group, real and effective", command_id },
	{ "run", "PROGRAM [ARGUMENTS] a process of its own, waited for", command_run },
	{ "spawn", "PROGRAM [ARGUMENTS] a process of its own, not waited for", command_spawn },
	{ "kill", "PID [SIGNAL] a signal to a process (PID !: the one spawned last)", command_kill },
	{ "wait", "the next of its processes to end", command_wait },
	{ "sleep", "SECONDS nothing, for that long", command_sleep },
	{ "fork", "[PROGRAM [ARGUMENTS]] a copy of this process; it may become PROGRAM", command_fork },
	{ "crash", "KIND this process stopped by a mistake (in a child: run)", command_crash },
	{ "alarm", "[COUNT] a timer's signals caught by a handler", command_alarm },
	{ "mask", "SIGUSR1 blocked, sent by a child, pending, then let through (in a child: run)", command_mask },
	{ "fds", "N... whether each descriptor is open here", command_fds },
	{ "cloexec", "FILE descriptors of it marked FD_CLOEXEC and not: a program started has only the unmarked", command_cloexec },
	{ "pipe", "WORDS... the words through a pipe", command_pipe },
	{ "unix-name", "PATH a socket by name in a file system: a child connects to it", command_unix_name },
	{ "shm", "[NAME] memory shared by name with a child (shm_open, mmap MAP_SHARED)", command_shm },
	{ "dgram", "[signal] datagrams through a socket pair: sizes kept, a peek, one cut, a send after shutdown", command_dgram },
	{ "unix", "FILE its descriptor sent through a socket pair, and read", command_unix },
	{ "threads", "[COUNT] threads sharing some work, timed against one", command_threads },
	{ "locks", "[COUNT] threads adding under one lock (ulock_wait, ulock_wake)", command_locks },
	{ "mach", "[COUNT] messages to itself through a Mach port, timed", command_mach },
	{ "fifo", "PATH a named pipe: a child writes into it, this process reads", command_fifo },
	{ "flock", "FILE the file locked: another process cannot have it, until it is let go", command_flock },
	{ "byte-locks", "FILE bytes of the file locked (fcntl): another process kept off them, not off others", command_byte_locks },
	{ "pipe-size", "how much a pipe holds, and when poll finds it writable", command_pipe_size },
	{ "watch", "FILE what kqueue says of the file as it is written, changed, linked, renamed, removed", command_watch },
	{ "poll", "poll and select on a pipe, empty and with a byte in it", command_poll },
	{ "kevent", "[COUNT] a timer and a pipe, waited for through kqueue", command_kevent },
	{ NULL, "IOKit", NULL },
	{ "ioreg", "[DEPTH] IOKit's registry, asked in MIG messages", command_ioreg },
	{ "ioprop", "PATH KEY a property of a registry entry", command_ioprop },
	{ "draw", "a picture on the screen, through IOSurface and the framebuffer", command_draw },
	{ NULL, "devices", NULL },
	{ "input", "[COUNT] what the touch screen and the buttons say", command_input },
	{ "tone", "HZ [SECONDS] a sine wave on the sound device", command_tone },
};

void
command_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (commands[i].name == NULL) {
			say(i == 0 ? "" : "\n");
			say(commands[i].what);
			say(":\n");
			continue;
		}
		say("  ");
		say(commands[i].name);
		for (size_t n = length(commands[i].name); n < 10; n++) {
			say(" ");
		}
		say(commands[i].what);
		say("\n");
	}
}

/* a command, its name first; whether there is one of that name */
static int
run_words(int argc, char **argv)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (commands[i].name != NULL && same(argv[0], commands[i].name)) {     /* (a NULL name: a heading of help's) */
			commands[i].run(argc, argv);
			return 1;
		}
	}
	say(argv[0]);
	say(": not a command here (help lists them)\n");
	return 0;
}

static void
run(char *text)
{
	char *argv[WORDS + 1];
	int argc = split(text, argv);

	if (argc > 0) {
		argv[argc] = NULL;
		run_words(argc, argv);
	}
}

/*
 * The kernel starts a static program with the stack holding argc, then argv and its NULL, then the
 * environment: _start hands the stack to the C side.
 */
__asm__(".globl _start\n_start:\n\tmov x0, sp\n\tb _start_with_stack\n");

void start_with_stack(long *stack);

void
start_with_stack(long *stack)
{
	int argc = (int)stack[0];
	char **argv = (char **)(stack + 1);
	char text[256];
	long fd = syscall6(SYS_open, (long)"/dev/console", O_RDWR | O_NOCTTY, 0, 0, 0, 0);

	if (fd >= 0) {
		console = (int)fd;
	}
	/* not the first process, with words: one command (run /sbin/launchd uptime), then the end */
	if (syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0) != 1 && argc > 1) {
		if (argv[1][0] == '>' && argc > 2) {      /* ">N": what it says goes to its descriptor N */
			console = (int)parse_number(argv[1] + 1);
			argc--;
			argv++;
		}
		syscall6(SYS_exit, run_words(argc - 1, argv + 1) ? 0 : 127, 0, 0, 0, 0, 0);
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
