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

void
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
	{ "sysctl", "NAME[=VALUE]... the kernel's value of each, set if given", command_sysctl },
	{ "echo", "WORDS... say them", command_echo },
	{ "ls", "[PATH...] what is in a directory", command_ls },
	{ "cat", "FILE... what is in a file", command_cat },
	{ "ps", "the processes", command_ps },
	{ "uptime", "how long the kernel has run", command_uptime },
	{ "ifconfig", "[NAME ADDRESS [MASK]] the interfaces, or an address for one", command_ifconfig },
	{ "ping", "ADDRESS [COUNT] ICMP echo: is it there", command_ping },
	{ "serve", "[COUNT] a page over HTTP on port 80 (or /bytes/N: N bytes)", command_serve },
	{ "get", "ADDRESS PORT [PATH] a page asked for over HTTP", command_get },
	{ "udp", "ADDRESS PORT WORDS... a datagram, and its answer", command_udp },
	{ "mount", "TYPE DEVICE DIR a disk's file system (read only), or ramfs", command_mount },
	{ "tone", "HZ [SECONDS] a sine wave on the sound device", command_tone },
	{ "input", "[COUNT] what the touch screen and the buttons say", command_input },
	{ "dmesg", "[LINES] the kernel's latest messages", command_dmesg },
	{ "ioreg", "[DEPTH] IOKit's registry, asked in MIG messages", command_ioreg },
	{ "ioprop", "PATH KEY a property of a registry entry", command_ioprop },
	{ "run", "PROGRAM [ARGUMENTS] a process of its own, waited for", command_run },
	{ "fork", "[PROGRAM [ARGUMENTS]] a copy of this process; it may become PROGRAM", command_fork },
	{ "mach", "[COUNT] messages to itself through a Mach port, timed", command_mach },
	{ "date", "the date and time, UTC", command_date },
	{ "sleep", "SECONDS nothing, for that long", command_sleep },
	{ "spawn", "PROGRAM [ARGUMENTS] a process of its own, not waited for", command_spawn },
	{ "kill", "PID [SIGNAL] a signal to a process (PID !: the one spawned last)", command_kill },
	{ "wait", "the next of its processes to end", command_wait },
	{ "crash", "KIND this process stopped by a mistake (in a child: run)", command_crash },
	{ "alarm", "[COUNT] a timer's signals caught by a handler", command_alarm },
	{ "pipe", "WORDS... the words through a pipe", command_pipe },
	{ "unix", "FILE its descriptor sent through a socket pair, and read", command_unix },
	{ "mkdir", "DIR... new directories", command_mkdir },
	{ "write", "FILE WORDS... the words as all of a file", command_write },
	{ "rm", "PATH... files, or empty directories, gone", command_rm },
	{ "threads", "[COUNT] threads sharing some work, timed against one", command_threads },
	{ "mmap", "FILE the file's pages, checked against its reads", command_mmap },
	{ "memory", "MB that much memory of its own, written and read back", command_memory },
	{ "kevent", "[COUNT] a timer and a pipe, waited for through kqueue", command_kevent },
};

void
command_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
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
		if (same(argv[0], commands[i].name)) {
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
