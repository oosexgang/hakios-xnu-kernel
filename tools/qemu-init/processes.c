/* tools/qemu-init: processes and their signals and descriptors: ps, run, spawn, kill, wait, sleep, kevent, crash,
 * alarm, fork, mask, fds, cloexec, id, watch */
#include "init.h"

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

void
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

#define SYS_wait4               7
#define SYS_posix_spawn         244
#define SYS_munmap              73
#define SYS_mmap                197
#define SYS_mprotect            74
#define PROT_RW                 3

/* how a process ended, as wait4 says it, after WHO ("run: "), in one line */
static void
say_ended(const char *who, int32_t pid, int32_t status)
{
	char line[96];
	size_t at = append(line, 0, sizeof(line), who);

	at = append_number(line, append(line, at, sizeof(line), "pid "), sizeof(line), (uint64_t)pid);
	if ((status & 0x7f) == 0) {
		at = append_number(line, append(line, at, sizeof(line), " exited with "), sizeof(line), (uint64_t)((status >> 8) & 0xff));
	} else {
		at = append_number(line, append(line, at, sizeof(line), " ended by signal "), sizeof(line), (uint64_t)(status & 0x7f));
	}
	append(line, at, sizeof(line), "\n");
	say(line);
}

/* PROGRAM [ARGUMENTS]: a new process (posix_spawn) running PROGRAM, waited for; what it ended with */
void
command_run(int argc, char **argv)
{
	char *environment[] = { NULL };
	int32_t pid = 0, status = 0;

	if (argc < 2) {
		say("run PROGRAM [ARGUMENTS]\n");
		return;
	}
	long error = syscall6(SYS_posix_spawn, (long)&pid, (long)argv[1], 0, (long)(argv + 1), (long)environment, 0);
	if (error != 0) {
		say("run: ");
		say(argv[1]);
		say(" does not start: ");
		say_error(error);
		say("\n");
		return;
	}
	if (syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0) != pid) {
		say("run: pid ");
		say_number((uint64_t)pid);
		say(" was not waited for\n");
		return;
	}
	say_ended("run: ", pid, status);
}

/* osfmk/kern/syscall_sw.c: Mach's traps are the negative numbers; they return a kern_return_t in x0 */
#define SYS_kill                37

/* SECONDS: nothing, for that long */
void
command_sleep(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? parse_number(argv[1]) : 0;

	if (seconds < 1 || seconds > 3600) {
		say("sleep SECONDS: 1 to 3600\n");
		return;
	}
	sleep_us((int64_t)seconds * 1000000);
}

static int32_t last_spawned;           /* (kill's "!") */

/* PROGRAM [ARGUMENTS]: a process of its own, not waited for (wait does, kill ends it) */
void
command_spawn(int argc, char **argv)
{
	char *environment[] = { NULL };
	int32_t pid = 0;

	if (argc < 2) {
		say("spawn PROGRAM [ARGUMENTS]\n");
		return;
	}
	long error = syscall6(SYS_posix_spawn, (long)&pid, (long)argv[1], 0, (long)(argv + 1), (long)environment, 0);
	if (error != 0) {
		say("spawn: ");
		say(argv[1]);
		say(" does not start: ");
		say_error(error);
		say("\n");
		return;
	}
	last_spawned = pid;
	say("spawn: pid ");
	say_number((uint64_t)pid);
	say("\n");
}

/* PID [SIGNAL]: a signal (15, SIGTERM, if not said) to a process; PID "!" is the one spawned last */
void
command_kill(int argc, char **argv)
{
	unsigned pid = argc > 1 ? (same(argv[1], "!") ? (unsigned)last_spawned : parse_number(argv[1])) : 0;
	unsigned signal = argc > 2 ? parse_number(argv[2]) : 15;

	if (pid < 2 || signal < 1 || signal > 31) {
		say("kill PID [SIGNAL]: a process other than this one, a signal from 1 to 31\n");
		return;
	}
	long error = syscall6(SYS_kill, pid, signal, 1, 0, 0, 0);
	if (error != 0) {
		say("kill: ");
		say_error(error);
		say("\n");
	}
}

/* the next of its processes to end */
void
command_wait(int argc, char **argv)
{
	int32_t status = 0;
	(void)argc;
	(void)argv;

	long pid = syscall6(SYS_wait4, -1, (long)&status, 0, 0, 0, 0);
	if (pid <= 0) {
		say("wait: no process of its own\n");
		return;
	}
	say_ended("wait: ", (int32_t)pid, status);
}

#define SYS_kqueue              362
#define SYS_kevent              363
#define EVFILT_READ             (-1)
#define EVFILT_TIMER            (-7)
#define EV_ADD                  1

struct kevent64 {                       /* bsd/sys/event.h's struct kevent, packed to 4 */
	uint64_t ident;
	int16_t filter;
	uint16_t flags;
	uint32_t fflags;
	int64_t data;
	uint64_t udata;
} __attribute__((packed, aligned(4)));

/* [COUNT]: a timer of 50 ms through kqueue, COUNT times (10 if not said), then a pipe seen to be readable */
void
command_kevent(int argc, char **argv)
{
	unsigned count = argc > 1 ? parse_number(argv[1]) : 10;
	struct kevent64 change = { .ident = 1, .filter = EVFILT_TIMER, .flags = EV_ADD, .data = 50 }, event;
	int ends[2];

	if (count < 1 || count > 1000) {
		say("kevent [COUNT]: 1 to 1000 timer events\n");
		return;
	}
	long kq = syscall6(SYS_kqueue, 0, 0, 0, 0, 0, 0);
	if (kq < 0 || syscall6(SYS_kevent, kq, (long)&change, 1, 0, 0, 0) < 0) {
		say("kevent: no kqueue, or it takes no timer\n");
		return;
	}
	int64_t start = now_us();
	unsigned fired = 0;
	while (fired < count) {
		if (syscall6(SYS_kevent, kq, 0, 0, (long)&event, 1, 0) != 1 || event.filter != EVFILT_TIMER) {
			break;
		}
		fired += (unsigned)event.data;          /* (how many times it fired since it was last seen) */
	}
	int64_t took = now_us() - start;
	say("kevent: ");
	say_number(fired);
	say(" timer events of 50 ms in ");
	say_number((uint64_t)took / 1000);
	say(" ms");
	/* a pipe: nothing to read, then five bytes, which the kqueue says are there */
	if (syscall_pipe(ends) == 0) {
		struct kevent64 readable = { .ident = (uint64_t)ends[0], .filter = EVFILT_READ, .flags = EV_ADD };

		change.flags = 2;       /* EV_DELETE: the timer is done with */
		syscall6(SYS_kevent, kq, (long)&change, 1, 0, 0, 0);
		syscall6(SYS_kevent, kq, (long)&readable, 1, 0, 0, 0);
		syscall6(SYS_write, ends[1], (long)"ready", 5, 0, 0, 0);
		if (syscall6(SYS_kevent, kq, 0, 0, (long)&event, 1, 0) == 1 && event.filter == EVFILT_READ && event.data == 5) {
			say("; a pipe with 5 bytes in it is readable");
		}
		syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
		syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	}
	say("\n");
	syscall6(SYS_close, kq, 0, 0, 0, 0, 0);
}

/*
 * KIND: this process stopped by the processor, as a program's mistake stops it: "null" reads address 0
 * (SIGSEGV), "illegal" runs an undefined instruction (SIGILL), "breakpoint" a brk (SIGTRAP). What the
 * kernel makes of the exception is what the one waiting for it sees (run /sbin/launchd crash null). Not
 * in the first process, whose end is the kernel's.
 */
void
command_crash(int argc, char **argv)
{
	if (syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0) == 1) {
		say("crash: not in the first process (run /sbin/launchd crash KIND)\n");
		return;
	}
	if (argc == 2 && same(argv[1], "null")) {
		say((const char *)*(volatile long *)0);
	} else if (argc == 2 && same(argv[1], "illegal")) {
		__asm__ volatile ("udf #0");
	} else if (argc == 2 && same(argv[1], "breakpoint")) {
		__asm__ volatile ("brk #0");
	} else if (argc == 2 && same(argv[1], "readonly")) {
		/* a page of its own, written, then made read only (mprotect) and written again */
		volatile long *page = (volatile long *)syscall6(SYS_mmap, 0, 16384, PROT_RW, 0x1002, -1, 0);   /* MAP_PRIVATE | MAP_ANON */

		if ((long)page < 0) {
			say("crash: no page\n");
			return;
		}
		page[0] = 1;
		syscall6(SYS_mprotect, (long)page, 16384, 1, 0, 0, 0);
		page[0] = 2;
	}
	say("crash KIND: null, illegal, breakpoint or readonly\n");
}

/* ---- signals caught: alarm ---- */

#define SYS_sigaction           46
#define SYS_setitimer           83
#define SYS_sigreturn           184
#define SIGALRM                 14
#define ITIMER_REAL             0

/*
 * Where the kernel sends a caught signal (sa_tramp): x0 the handler, x1 the context's kind, x2 the
 * signal, x3 its information, x4 the context, x5 the token sigreturn must be shown. The handler runs,
 * then sigreturn puts the thread back where the signal took it.
 */
__asm__(".globl _signal_trampoline\n_signal_trampoline:\n"
        "\tmov x19, x0\n\tmov x20, x1\n\tmov x21, x4\n\tmov x22, x5\n"
        "\tmov x0, x2\n\tmov x1, x3\n\tmov x2, x4\n\tblr x19\n"
        "\tmov x0, x21\n\tmov x1, x20\n\tmov x2, x22\n\tmov x16, #184\n\tsvc #0x80\n\tbrk #1\n");
void signal_trampoline(void);

struct sigaction_in {                   /* bsd/sys/signal.h's struct __sigaction */
	void (*handler)(int);
	void (*trampoline)(void);
	uint32_t mask;
	int32_t flags;
};

struct itimerval {
	struct { int64_t seconds; int32_t microseconds, pad; } interval, value;
};

static volatile uint32_t alarms;

static void
on_alarm(int signal)
{
	(void)signal;
	alarms++;
}

/* [COUNT]: a timer of a tenth of a second (setitimer) whose SIGALRM a handler catches, COUNT times */
void
command_alarm(int argc, char **argv)
{
	unsigned count = argc > 1 ? parse_number(argv[1]) : 10;
	struct sigaction_in action = { on_alarm, signal_trampoline, 0, 0 }, none = { 0, 0, 0, 0 };
	struct itimerval every = { { 0, 100000, 0 }, { 0, 100000, 0 } }, stop = { { 0, 0, 0 }, { 0, 0, 0 } };

	if (count < 1 || count > 100) {
		say("alarm [COUNT]: 1 to 100\n");
		return;
	}
	if (syscall6(SYS_sigaction, SIGALRM, (long)&action, 0, 0, 0, 0) != 0) {
		say("alarm: the signal cannot be caught\n");
		return;
	}
	alarms = 0;
	int64_t start = now_us();
	syscall6(SYS_setitimer, ITIMER_REAL, (long)&every, 0, 0, 0, 0);
	for (int waited = 0; alarms < count && waited < (int)count * 4; waited++) {
		syscall6(SYS_sigsuspend, 0, 0, 0, 0, 0, 0);     /* (until a signal is caught) */
	}
	syscall6(SYS_setitimer, ITIMER_REAL, (long)&stop, 0, 0, 0, 0);
	int64_t took = now_us() - start;
	syscall6(SYS_sigaction, SIGALRM, (long)&none, 0, 0, 0, 0);
	say("alarm: ");
	say_number(alarms);
	say(" SIGALRM caught by a handler in ");
	say_number((uint64_t)took / 1000);
	say(" ms\n");
}

/* ---- signals held back: a mask ---- */

#define SYS_getppid             39
#define SYS_sigprocmask         48
#define SYS_sigpending          52
#define SIGUSR1                 30
#define SIG_BLOCK               1
#define SIG_UNBLOCK             2

static volatile uint32_t usr1s;

static void
on_usr1(int signal)
{
	(void)signal;
	usr1s++;
}

/*
 * SIGUSR1 blocked (sigprocmask), then sent by a forked child: it waits, pending (sigpending), its handler not
 * run; let through, the handler runs, once
 */
void
command_mask(int argc, char **argv)
{
	struct sigaction_in action = { on_usr1, signal_trampoline, 0, 0 }, none = { 0, 0, 0, 0 };
	uint32_t usr1 = 1u << (SIGUSR1 - 1), was = 0, pending = 0;
	int in_child = 0;
	int32_t status = 0;
	uint32_t before, after;
	long pid;
	char line[200];
	size_t at;

	(void)argc;
	(void)argv;
	if (syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0) == 1) {
		say("mask: not in the first process, which nobody may signal (run /sbin/launchd mask)\n");
		return;
	}
	if (syscall6(SYS_sigaction, SIGUSR1, (long)&action, 0, 0, 0, 0) != 0) {
		say("mask: SIGUSR1 cannot be caught\n");
		return;
	}
	usr1s = 0;
	syscall6(SYS_sigprocmask, SIG_BLOCK, (long)&usr1, (long)&was, 0, 0, 0);
	pid = syscall_fork(&in_child);
	if (in_child) {
		syscall6(SYS_kill, syscall6(SYS_getppid, 0, 0, 0, 0, 0, 0), SIGUSR1, 1, 0, 0, 0);
		syscall6(SYS_exit, 0, 0, 0, 0, 0, 0);
	}
	if (pid > 0) {
		syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	}
	syscall6(SYS_sigpending, (long)&pending, 0, 0, 0, 0, 0);
	before = usr1s;
	syscall6(SYS_sigprocmask, SIG_UNBLOCK, (long)&usr1, 0, 0, 0, 0);
	after = usr1s;
	syscall6(SYS_sigprocmask, 3 /* SIG_SETMASK */, (long)&was, 0, 0, 0, 0);
	syscall6(SYS_sigaction, SIGUSR1, (long)&none, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "mask: SIGUSR1 from pid ");
	at = append_number(line, at, sizeof(line), (uint64_t)(pid > 0 ? pid : 0));
	at = append(line, at, sizeof(line), (pending & usr1) && before == 0 ? ": held while blocked (pending, not caught)" :
	    ": NOT held while blocked");
	at = append(line, at, sizeof(line), after == 1 ? ", caught once let through\n" : ", NOT caught once let through\n");
	syscall6(SYS_write, console, (long)line, (long)at, 0, 0, 0);
}

/* ---- fork: a copy of this process ---- */

#define SYS_execve              59


static volatile uint64_t forked_value = 1;

/*
 * [PROGRAM [ARGUMENTS]]: a copy of this process (fork). The child changes a value of its memory, which
 * the parent's copy must not see, and exits with what it saw; or, given a program, becomes it (execve).
 */
void
command_fork(int argc, char **argv)
{
	char *environment[] = { NULL };
	int in_child = 0;
	int32_t status = 0;

	forked_value = 1;
	long pid = syscall_fork(&in_child);
	if (pid < 0) {
		say("fork: no copy: ");
		say_error(pid);
		say("\n");
		return;
	}
	if (in_child) {
		forked_value = 2;               /* the child's page, copied on this write: not the parent's */
		if (argc > 1) {
			long error = syscall6(SYS_execve, (long)argv[1], (long)(argv + 1), (long)environment, 0, 0, 0);

			say("fork: the child cannot become ");
			say(argv[1]);
			say(": ");
			say_error(error);
			say("\n");
			syscall6(SYS_exit, 127, 0, 0, 0, 0, 0);
		}
		syscall6(SYS_exit, (long)(40 + forked_value), 0, 0, 0, 0, 0);
	}
	if (syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0) != pid) {
		say("fork: the child was not waited for\n");
		return;
	}
	char line[128];
	size_t at = append_number(line, append(line, 0, sizeof(line), "fork: child "), sizeof(line), (uint64_t)pid);
	if (argc > 1) {
		at = append_number(line, append(line, at, sizeof(line), " ran it and exited with "), sizeof(line),
		    (uint64_t)((status >> 8) & 0xff));
	} else {
		at = append_number(line, append(line, at, sizeof(line), " saw "), sizeof(line), (uint64_t)((status >> 8) & 0xff) - 40);
		at = append_number(line, append(line, at, sizeof(line), " in its copy, the parent still "), sizeof(line), forked_value);
	}
	append(line, at, sizeof(line), "\n");
	say(line);
}

#define SYS_fcntl               92
#define F_GETFD                 1
#define FD_CLOEXEC              1
#define O_CLOEXEC               0x1000000
#define F_DUPFD_CLOEXEC         67
#define SYS_dup2                90

/* N...: whether each descriptor is open in this process (fcntl F_GETFD); what a program started with them has */
void
command_fds(int argc, char **argv)
{
	char line[160];
	size_t at = append(line, 0, sizeof(line), "fds:");

	if (argc < 2) {
		say("fds N...\n");
		return;
	}
	for (int i = 1; i < argc && at < sizeof(line) - 24; i++) {
		at = append(line, at, sizeof(line), " ");
		at = append(line, at, sizeof(line), argv[i]);
		at = append(line, at, sizeof(line),
		    syscall6(SYS_fcntl, parse_number(argv[i]), F_GETFD, 0, 0, 0, 0) >= 0 ? " open," : " closed,");
	}
	append(line, at - 1, sizeof(line), "\n");
	say(line);
}

/* FILE: the file opened with O_CLOEXEC (so marked FD_CLOEXEC), and three descriptors made of it and of a plain one:
 * 10 by dup2 of the plain one, 11 by F_DUPFD_CLOEXEC (marked), 12 by dup2 of the marked one (which leaves the
 * mark behind); a program started (posix_spawn) has 10 and 12, and not 11, which closed as it started. (Numbers
 * well up: the program's own first open, of its console, takes the lowest free.) */
void
command_cloexec(int argc, char **argv)
{
	char *environment[] = { NULL };
	char *child[] = { "/sbin/launchd", "fds", "10", "11", "12", NULL };
	int32_t pid = 0, status = 0;
	long plain, marked, error;
	int ok;

	if (argc != 2) {
		say("cloexec FILE\n");
		return;
	}
	plain = syscall6(SYS_open, (long)argv[1], 0, 0, 0, 0, 0);
	marked = syscall6(SYS_open, (long)argv[1], O_CLOEXEC, 0, 0, 0, 0);
	if (plain < 0 || marked < 0) {
		say_failed("cloexec", argv[1], plain < 0 ? plain : marked);
	} else {
		ok = syscall6(SYS_fcntl, marked, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC &&
		    syscall6(SYS_dup2, plain, 10, 0, 0, 0, 0) == 10 && syscall6(SYS_fcntl, 10, F_GETFD, 0, 0, 0, 0) == 0 &&
		    syscall6(SYS_fcntl, marked, F_DUPFD_CLOEXEC, 11, 0, 0, 0) == 11 &&
		    syscall6(SYS_fcntl, 11, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC &&
		    syscall6(SYS_dup2, marked, 12, 0, 0, 0, 0) == 12 && syscall6(SYS_fcntl, 12, F_GETFD, 0, 0, 0, 0) == 0;
		say(ok ? "cloexec: O_CLOEXEC and F_DUPFD_CLOEXEC mark a descriptor FD_CLOEXEC, dup and dup2 do not\n" :
		    "cloexec: the descriptors are NOT marked as BSD marks them\n");
		if ((error = syscall6(SYS_posix_spawn, (long)&pid, (long)child[0], 0, (long)child, (long)environment, 0)) != 0) {
			say("cloexec: no program started: ");
			say_error(error);
			say("\n");
		} else {
			syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
		}
	}
	for (long fd = 10; fd <= 12; fd++) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	if (plain >= 0) {
		syscall6(SYS_close, plain, 0, 0, 0, 0, 0);
	}
	if (marked >= 0) {
		syscall6(SYS_close, marked, 0, 0, 0, 0, 0);
	}
}

#define SYS_getuid              24
#define SYS_geteuid             25
#define SYS_getgid              47
#define SYS_getegid             43

/* who this process is: its real and effective user and group */
void
command_id(int argc, char **argv)
{
	char line[96];
	size_t at;

	(void)argc;
	(void)argv;
	at = append(line, 0, sizeof(line), "id: user ");
	at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_getuid, 0, 0, 0, 0, 0, 0));
	at = append(line, at, sizeof(line), ", effective ");
	at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_geteuid, 0, 0, 0, 0, 0, 0));
	at = append(line, at, sizeof(line), "; group ");
	at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_getgid, 0, 0, 0, 0, 0, 0));
	at = append(line, at, sizeof(line), ", effective ");
	at = append_number(line, at, sizeof(line), (uint64_t)syscall6(SYS_getegid, 0, 0, 0, 0, 0, 0));
	append(line, at, sizeof(line), "\n");
	say(line);
}

#define EVFILT_VNODE            (-4)
#define EV_CLEAR                0x20
#define NOTE_DELETE             0x01
#define NOTE_WRITE              0x02
#define NOTE_EXTEND             0x04
#define NOTE_ATTRIB             0x08
#define NOTE_LINK               0x10
#define NOTE_RENAME             0x20
#define SYS_link                9
#define SYS_unlink              10
#define SYS_chmod               15
#define SYS_rename              128

/* the events one kevent() gave on the file within a second, as names */
static void
watched(char *line, size_t *at, size_t size, long kq, const char *what)
{
	struct kevent64 got;
	struct { int64_t seconds; long nanoseconds; } second = { 1, 0 };
	static const struct { uint32_t bit; const char *name; } notes[] = {
		{ NOTE_DELETE, "delete" }, { NOTE_WRITE, "write" }, { NOTE_EXTEND, "extend" }, { NOTE_ATTRIB, "attrib" },
		{ NOTE_LINK, "link" }, { NOTE_RENAME, "rename" },
	};
	long n = syscall6(SYS_kevent, kq, 0, 0, (long)&got, 1, (long)&second);

	*at = append(line, *at, size, *at > 7 ? "; " : " ");
	*at = append(line, *at, size, what);
	*at = append(line, *at, size, ":");
	if (n != 1) {
		*at = append(line, *at, size, " NOTHING");
		return;
	}
	for (unsigned i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
		if (got.fflags & notes[i].bit) {
			*at = append(line, *at, size, " ");
			*at = append(line, *at, size, notes[i].name);
		}
	}
}

/* FILE: the file watched through kqueue (EVFILT_VNODE) while it is written, added to, changed in mode, linked,
 * renamed and removed; what each gives */
void
command_watch(int argc, char **argv)
{
	struct kevent64 change;
	struct { int64_t seconds; long nanoseconds; } no_wait = { 0, 0 };
	char line[300], other[256], moved[256];
	size_t at;
	long kq, fd;

	if (argc != 2 || length(argv[1]) > sizeof(other) - 8) {
		say("watch FILE (made anew)\n");
		return;
	}
	at = append(other, 0, sizeof(other), argv[1]);
	append(other, at, sizeof(other), "-link");
	append(moved, append(moved, 0, sizeof(moved), argv[1]), sizeof(moved), "-moved");
	syscall6(SYS_unlink, (long)argv[1], 0, 0, 0, 0, 0);
	if ((fd = syscall6(SYS_open, (long)argv[1], O_WRONLY | 0x200 /* O_CREAT */, 0644, 0, 0, 0)) < 0 ||
	    (kq = syscall6(SYS_kqueue, 0, 0, 0, 0, 0, 0)) < 0) {
		say("watch: no file, or no kqueue\n");
		return;
	}
	change = (struct kevent64){ .ident = (uint64_t)fd, .filter = EVFILT_VNODE, .flags = EV_ADD | EV_CLEAR,
		                    .fflags = NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME };
	if (syscall6(SYS_kevent, kq, (long)&change, 1, 0, 0, 0) != 0) {
		say("watch: the file cannot be watched\n");
		syscall6(SYS_close, kq, 0, 0, 0, 0, 0);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	at = append(line, 0, sizeof(line), "watch:");
	syscall6(SYS_write, fd, (long)"more", 4, 0, 0, 0);
	watched(line, &at, sizeof(line), kq, "written");
	syscall6(SYS_chmod, (long)argv[1], 0600, 0, 0, 0, 0);
	watched(line, &at, sizeof(line), kq, "chmod");
	syscall6(SYS_link, (long)argv[1], (long)other, 0, 0, 0, 0);
	watched(line, &at, sizeof(line), kq, "linked");
	syscall6(SYS_unlink, (long)other, 0, 0, 0, 0, 0);
	(void)syscall6(SYS_kevent, kq, 0, 0, (long)&change, 1, (long)&no_wait);      /* (the second link's going: not watched) */
	syscall6(SYS_rename, (long)argv[1], (long)moved, 0, 0, 0, 0);
	watched(line, &at, sizeof(line), kq, "renamed");
	syscall6(SYS_unlink, (long)moved, 0, 0, 0, 0, 0);
	watched(line, &at, sizeof(line), kq, "removed");
	append(line, at, sizeof(line), "\n");
	say(line);
	syscall6(SYS_close, kq, 0, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}
