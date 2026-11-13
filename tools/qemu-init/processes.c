/* tools/qemu-init: processes, threads, signals, pipes, Mach messages and kqueue */
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
		say(" does not start (error ");
		say_number((uint64_t)-error);
		say(")\n");
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
#define TRAP_port_allocate      -16
#define TRAP_port_mod_refs      -19
#define TRAP_task_self          -28
#define TRAP_mach_msg2          -47
#define MACH_PORT_RIGHT_RECEIVE 1
#define MACH_MSG_TYPE_MAKE_SEND 20
#define MACH64_SEND_MSG         0x1ull
#define MACH64_RCV_MSG          0x2ull
#define MACH64_SEND_MQ_CALL     0x400000000ull  /* (mach_msg2: what the destination is, a message queue) */

struct mach_message {
	uint32_t bits, size, remote_port, local_port, voucher_port;
	int32_t id;
	uint64_t number;
	uint8_t trailer[200];           /* what the kernel adds on receiving */
};

/* [COUNT]: COUNT messages (1000 if not said) sent to a port and received from it, and the time each took */
void
command_mach(int argc, char **argv)
{
	unsigned count = argc > 1 ? parse_number(argv[1]) : 1000;
	uint32_t self = (uint32_t)trap8(TRAP_task_self, 0, 0, 0, 0, 0, 0, 0, 0), port = 0;
	static struct mach_message message;

	if (count < 1 || count > 1000000) {
		say("mach [COUNT]: 1 to 1000000 messages\n");
		return;
	}
	long kr = trap8(TRAP_port_allocate, self, MACH_PORT_RIGHT_RECEIVE, (long)&port, 0, 0, 0, 0, 0);
	if (kr != 0) {
		say("mach: no port (error ");
		say_number((uint64_t)kr);
		say(")\n");
		return;
	}
	int64_t start = now_us();
	unsigned done = 0;
	const uint32_t size = 32;       /* the header and the number */

	for (; done < count; done++) {
		message.bits = MACH_MSG_TYPE_MAKE_SEND;         /* (the remote port's right, made from the receive right) */
		message.size = size;
		message.remote_port = port;
		message.local_port = message.voucher_port = 0;
		message.id = (int32_t)done;
		message.number = 0x6d616368ull * done;
		kr = trap8(TRAP_mach_msg2, (long)&message, (long)(MACH64_SEND_MSG | MACH64_RCV_MSG | MACH64_SEND_MQ_CALL),
		    (long)(MACH_MSG_TYPE_MAKE_SEND | (uint64_t)size << 32), port, (long)((uint64_t)done << 32),
		    (long)((uint64_t)port << 32), sizeof(message), 0);
		if (kr != 0 || message.id != (int32_t)done || message.number != 0x6d616368ull * done) {
			say("mach: message ");
			say_number(done);
			say(kr != 0 ? " was not sent or received (error " : " came back other than it went (");
			say_number((uint64_t)kr);
			say(")\n");
			break;
		}
	}
	int64_t took = now_us() - start;
	trap8(TRAP_port_mod_refs, self, port, MACH_PORT_RIGHT_RECEIVE, -1, 0, 0, 0, 0);
	if (done == count) {
		say("mach: ");
		say_number(count);
		say(" messages to itself through a port, ");
		say_number((uint64_t)(took * 1000 / count));
		say(" ns each\n");
	}
}

#define SYS_kill                37
#define SYS_pipe                42

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
		say(" does not start (error ");
		say_number((uint64_t)-error);
		say(")\n");
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
		say("kill: error ");
		say_number((uint64_t)-error);
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

/* pipe() gives its two ends in x0 and x1 */
static long
syscall_pipe(int ends[2])
{
	register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0;
	register long x16 __asm__("x16") = SYS_pipe;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs" : "+r"(x0), "+r"(x1), [failed] "=r"(failed) : "r"(x16) : "memory", "cc");
	ends[0] = (int)x0;
	ends[1] = (int)x1;
	return failed ? -x0 : 0;
}

/* WORDS...: the words through a pipe: a child (this program, "echo") writes them into it, this process reads */
void
command_pipe(int argc, char **argv)
{
	static char text[4096];
	char *environment[] = { NULL }, *child[WORDS + 4] = { "/sbin/launchd", NULL, "echo" }, to[16] = ">";
	int ends[2], c = 3;
	int32_t pid = 0, status = 0;

	if (syscall_pipe(ends) != 0) {
		say("pipe: no pipe\n");
		return;
	}
	/* (">N": the child says what it says into its descriptor N, which is the pipe's end it inherits) */
	to[1] = (char)('0' + ends[1] / 10);
	to[2] = (char)('0' + ends[1] % 10);
	child[1] = to;
	for (int i = 1; i < argc && c < WORDS + 3; i++) {
		child[c++] = argv[i];
	}
	child[c] = NULL;
	long error = syscall6(SYS_posix_spawn, (long)&pid, (long)child[0], 0, (long)child, (long)environment, 0);
	syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);    /* (the child's copy is the one left: its end is the pipe's) */
	long all = 0, got;
	while (error == 0 && all < (long)sizeof(text) - 1 &&
	    (got = syscall6(SYS_read, ends[0], (long)text + all, (long)sizeof(text) - 1 - all, 0, 0, 0)) > 0) {
		all += got;
	}
	syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
	if (error != 0) {
		say("pipe: the child does not start\n");
		return;
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	text[all] = '\0';
	say("pipe: ");
	say_number((uint64_t)all);
	say(" bytes from pid ");
	say_number((uint64_t)pid);
	say(": ");
	say(text);
}

#define SYS_bsdthread_create    360
#define SYS_bsdthread_terminate 361
#define SYS_bsdthread_register  366
#define PTHREAD_START_CUSTOM    0x01000000      /* the stack and the thread's memory are the caller's */
#define THREADS_MAX             8
#define THREAD_STACK            (64 * 1024)

/*
 * Where the kernel starts a thread (bsdthread_register): x0 its memory, x1 its port, x2 the function,
 * x3 the function's argument, sp its stack. The function runs, then the thread ends, giving back the
 * port's right (bsdthread_terminate).
 */
__asm__(".globl _thread_entry\n_thread_entry:\n"
        "\tmov x19, x1\n\tmov x0, x3\n\tblr x2\n"
        "\tmov x0, #0\n\tmov x1, #0\n\tmov x2, x19\n\tmov x3, #0\n\tmov x16, #361\n\tsvc #0x80\n\tbrk #1\n");

void thread_entry(void);

static volatile uint64_t threads_total;

static volatile uint32_t threads_done;

static uint32_t thread_rounds;

/* a share of the work: count, then say so */
static void
thread_work(void *argument)
{
	uint64_t mine = 0;
	(void)argument;

	for (uint32_t i = 0; i < thread_rounds; i++) {
		mine += i & 1;
		__asm__ volatile ("" : "+r"(mine));     /* (not to be folded into one sum) */
	}
	__atomic_fetch_add(&threads_total, mine, __ATOMIC_RELAXED);
	__atomic_fetch_add(&threads_done, 1, __ATOMIC_RELEASE);
}

/* whether this process may start threads: its entry told to the kernel, once (a second time is refused) */
static int
threads_ready(void)
{
	static int registered;

	if (!registered && syscall6(SYS_bsdthread_register, (long)thread_entry, 0, 0, 0, 0, 0) >= 0) {
		registered = 1;         /* (what the kernel answers is its features) */
	}
	return registered;
}

/* COUNT threads doing WORK of ROUNDS each; how long it took; -1 if they did not all end */
static int64_t
threads_run(unsigned count, uint32_t rounds, void (*work)(void *))
{
	static uint8_t stacks[THREADS_MAX][THREAD_STACK] __attribute__((aligned(16)));
	static uint8_t memory[THREADS_MAX][4096] __attribute__((aligned(16)));
	int64_t start = now_us();

	threads_total = 0;
	threads_done = 0;
	thread_rounds = rounds;
	for (unsigned i = 0; i < count; i++) {
		if (syscall6(SYS_bsdthread_create, (long)work, 0, (long)(stacks[i] + THREAD_STACK), (long)memory[i],
		    PTHREAD_START_CUSTOM, 0) < 0) {
			say("threads: a thread does not start\n");
			return -1;
		}
	}
	for (int waited = 0; __atomic_load_n(&threads_done, __ATOMIC_ACQUIRE) < count; waited++) {
		if (waited == 6000) {           /* a minute */
			return -1;
		}
		sleep_us(10000);
	}
	return now_us() - start;
}

/* [COUNT]: COUNT threads (as many as processors, if not said) share some counting; how long it took them */
void
command_threads(int argc, char **argv)
{
	uint32_t processors = 1;
	size_t size = sizeof(processors);
	const uint32_t work = 1u << 28;     /* (half a second for one thread, in QEMU: well above the noise) */

	syscall6(SYS_sysctlbyname, (long)"hw.ncpu", 7, (long)&processors, (long)&size, 0, 0);
	unsigned count = argc > 1 ? parse_number(argv[1]) : processors;
	if (count < 1 || count > THREADS_MAX) {
		say("threads [COUNT]: 1 to 8\n");
		return;
	}
	if (!threads_ready()) {
		say("threads: the kernel takes no threads from this process\n");
		return;
	}
	int64_t alone = threads_run(1, work, thread_work), shared = threads_run(count, work / count, thread_work);
	if (alone < 0 || shared < 0 || threads_total != (uint64_t)(work / count / 2) * count) {
		say("threads: the work was not all done\n");
		return;
	}
	say("threads: ");
	say_number(count);
	say(count == 1 ? " thread on " : " threads on ");
	say_number(processors);
	say(processors == 1 ? " processor did in " : " processors did in ");
	say_number((uint64_t)shared / 1000);
	say(" ms what one does in ");
	say_number((uint64_t)alone / 1000);
	say(" ms\n");
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
	}
	say("crash KIND: null, illegal or breakpoint\n");
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

/* ---- a socket pair, and a descriptor sent through it ---- */

#define SYS_recvmsg             27
#define SYS_sendmsg             28
#define SYS_socketpair          135
#define AF_UNIX                 1
#define SOCK_STREAM             1
#define SOL_SOCKET              0xffff
#define SCM_RIGHTS              1

struct iovec {
	void *base;
	size_t length;
};

struct msghdr {
	void *name;
	uint32_t name_length;
	struct iovec *iov;
	int32_t iov_count;
	void *control;
	uint32_t control_length;
	int32_t flags;
};

struct rights {                         /* a cmsghdr with one descriptor (aligned to 4, as Darwin's are) */
	uint32_t length;
	int32_t level, type, fd;
};

/* FILE: FILE opened, its descriptor sent over a socket pair (SCM_RIGHTS) with a word, and read through
 * the descriptor that comes out of the other end */
void
command_unix(int argc, char **argv)
{
	char word[16] = "sent", got[16], text[128];
	int32_t ends[2];

	if (argc != 2) {
		say("unix FILE\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)argv[1], 0, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, (long)ends, 0, 0) != 0) {
		say(fd < 0 ? "unix: the file cannot be read\n" : "unix: no socket pair\n");
		return;
	}
	struct iovec out = { word, 5 }, in = { got, sizeof(got) };
	struct rights sent = { sizeof(sent), SOL_SOCKET, SCM_RIGHTS, (int32_t)fd }, came = { 0, 0, 0, -1 };
	struct msghdr message = { 0, 0, &out, 1, &sent, sizeof(sent), 0 };
	long n = syscall6(SYS_sendmsg, ends[0], (long)&message, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);         /* (the one in flight keeps the file open) */
	struct msghdr received = { 0, 0, &in, 1, &came, sizeof(came), 0 };
	if (n == 5) {
		n = syscall6(SYS_recvmsg, ends[1], (long)&received, 0, 0, 0, 0);
	}
	syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
	syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	if (n != 5 || came.type != SCM_RIGHTS || came.fd < 0) {
		say("unix: the descriptor did not come through\n");
		return;
	}
	long length = syscall6(SYS_read, came.fd, (long)text, sizeof(text) - 1, 0, 0, 0);
	syscall6(SYS_close, came.fd, 0, 0, 0, 0, 0);
	text[length > 0 ? length : 0] = '\0';
	for (long i = 0; i < length; i++) {
		if (text[i] == '\n') {
			text[i] = '\0';
			break;
		}
	}
	char line[200];
	size_t at = append(line, 0, sizeof(line), "unix: \"");
	at = append(line, at, sizeof(line), got);
	at = append_number(line, append(line, at, sizeof(line), "\" and descriptor "), sizeof(line), (uint64_t)came.fd);
	at = append(line, append(line, at, sizeof(line), " came through; it reads: "), sizeof(line), text);
	append(line, at, sizeof(line), "\n");
	say(line);
}

/* ---- fork: a copy of this process ---- */

#define SYS_fork                2
#define SYS_execve              59

/* fork() answers in x0 (the other's pid) and x1 (1 in the child); -errno if it failed */
static long
syscall_fork(int *in_child)
{
	register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0;
	register long x16 __asm__("x16") = SYS_fork;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs" : "+r"(x0), "+r"(x1), [failed] "=r"(failed) : "r"(x16) : "memory", "cc");
	*in_child = !failed && x1 != 0;
	return failed ? -x0 : x0;
}

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
		say("fork: no copy (error ");
		say_number((uint64_t)-pid);
		say(")\n");
		return;
	}
	if (in_child) {
		forked_value = 2;               /* the child's page, copied on this write: not the parent's */
		if (argc > 1) {
			syscall6(SYS_execve, (long)argv[1], (long)(argv + 1), (long)environment, 0, 0, 0);
			say("fork: the child cannot become ");
			say(argv[1]);
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

/* ---- locks: threads that take turns, as os_unfair_lock makes them, through ulock_wait and ulock_wake ---- */

#define SYS_ulock_wait          515
#define SYS_ulock_wake          516
#define UL_COMPARE_AND_WAIT     1

static volatile uint32_t lock_word;     /* 0 free, 1 held, 2 held and somebody waits */
static uint64_t locked_count;           /* only ever changed with the lock held: not atomic */
static volatile uint32_t lock_waits;

static void
lock_take(void)
{
	uint32_t was = 0;

	if (__atomic_compare_exchange_n(&lock_word, &was, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return;
	}
	while (__atomic_exchange_n(&lock_word, 2, __ATOMIC_ACQUIRE) != 0) {
		/* asleep in the kernel for as long as the word is 2 (what it is when the holder lets go is woken) */
		syscall6(SYS_ulock_wait, UL_COMPARE_AND_WAIT, (long)&lock_word, 2, 0, 0, 0);
		__atomic_fetch_add(&lock_waits, 1, __ATOMIC_RELAXED);
	}
}

static void
lock_give(void)
{
	if (__atomic_exchange_n(&lock_word, 0, __ATOMIC_RELEASE) == 2) {
		syscall6(SYS_ulock_wake, UL_COMPARE_AND_WAIT, (long)&lock_word, 0, 0, 0, 0);
	}
}

static void
locked_work(void *argument)
{
	(void)argument;
	for (uint32_t i = 0; i < thread_rounds; i++) {
		lock_take();
		uint64_t now = locked_count;
		__asm__ volatile ("" ::: "memory");    /* (a window for another thread, were the lock not one) */
		locked_count = now + 1;
		lock_give();
	}
	__atomic_fetch_add(&threads_done, 1, __ATOMIC_RELEASE);
}

/* [COUNT]: COUNT threads (as many as processors, if not said) add to one count under one lock; none lost */
void
command_locks(int argc, char **argv)
{
	uint32_t processors = 1;
	size_t size = sizeof(processors);
	const uint32_t rounds = 200000;

	syscall6(SYS_sysctlbyname, (long)"hw.ncpu", 7, (long)&processors, (long)&size, 0, 0);
	unsigned count = argc > 1 ? parse_number(argv[1]) : (processors > 1 ? processors : 2);
	if (count < 2 || count > THREADS_MAX) {
		say("locks [COUNT]: 2 to 8 threads\n");
		return;
	}
	if (!threads_ready()) {
		say("locks: the kernel takes no threads from this process\n");
		return;
	}
	locked_count = 0;
	lock_waits = 0;
	int64_t took = threads_run(count, rounds, locked_work);
	char line[160];
	size_t at = append(line, 0, sizeof(line), "locks: ");
	at = append_number(line, at, sizeof(line), count);
	at = append_number(line, append(line, at, sizeof(line), " threads added "), sizeof(line), locked_count);
	at = append_number(line, append(line, at, sizeof(line), " of "), sizeof(line), (uint64_t)count * rounds);
	at = append_number(line, append(line, at, sizeof(line), " under one lock, waited in the kernel "), sizeof(line), lock_waits);
	at = append_number(line, append(line, at, sizeof(line), " times, in "), sizeof(line), took < 0 ? 0 : (uint64_t)took / 1000);
	append(line, at, sizeof(line), took < 0 ? " ms: THEY DID NOT ALL END\n" : " ms\n");
	say(line);
}
