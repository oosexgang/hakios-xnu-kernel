/* tools/qemu-init: between processes -- a pipe, a socket pair and a descriptor through it, a socket by name,
 * memory shared by name, Mach messages, waiting on descriptors, a named pipe, a lock on a file: pipe, unix,
 * unix-name, shm, mach, poll, fifo, flock, byte-locks, pipe-size, dgram */
#include "init.h"

#define SYS_wait4               7
#define SYS_unlink              10
#define SYS_posix_spawn         244
#define SYS_munmap              73
#define SYS_mmap                197
#define SYS_ftruncate           201
#define SYS_shm_open            266
#define SYS_shm_unlink          267
#define O_RDWR                  2
#define O_CREAT_EXCL            (0x200 | 0x800)
#define PROT_RW                 3
#define MAP_SHARED              1

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

#define SYS_socket              97
#define SYS_connect             98
#define SYS_accept              30
#define SYS_bind                104
#define SYS_listen              106
#define SYS_lstat64             340

struct sockaddr_un {
	uint8_t len, family;
	char path[104];
};

/*
 * PATH: a socket by name in a file system (AF_UNIX, bound to PATH, where the file system keeps a node of
 * its own for it): a forked child connects to it by the name and writes, the shell reads what it wrote
 */
void
command_unix_name(int argc, char **argv)
{
	struct sockaddr_un here = { sizeof(here), AF_UNIX, { 0 } };
	struct { int32_t dev; uint16_t mode, nlink; char rest[200]; } st;
	char got[64], line[200];
	int in_child = 0;
	int32_t status = 0;
	long listener, peer, n = 0, pid;
	size_t at;

	if (argc != 2 || length(argv[1]) >= sizeof(here.path)) {
		say("unix-name PATH\n");
		return;
	}
	copy_bytes(here.path, argv[1], length(argv[1]) + 1);
	listener = syscall6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
	n = listener < 0 ? listener : syscall6(SYS_bind, listener, (long)&here, sizeof(here), 0, 0, 0);
	if (n == 0) {
		n = syscall6(SYS_listen, listener, 1, 0, 0, 0, 0);
	}
	if (n != 0) {
		say("unix-name: ");
		say(argv[1]);
		say(" cannot be bound: ");
		say_error(n);
		say("\n");
		if (listener >= 0) {
			syscall6(SYS_close, listener, 0, 0, 0, 0, 0);
		}
		return;
	}
	pid = syscall_fork(&in_child);
	if (in_child) {
		long sock = syscall6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);

		if (sock >= 0 && syscall6(SYS_connect, sock, (long)&here, sizeof(here), 0, 0, 0) == 0) {
			syscall6(SYS_write, sock, (long)"by its name", 11, 0, 0, 0);
		}
		syscall6(SYS_exit, 0, 0, 0, 0, 0, 0);
	}
	n = 0;
	peer = pid > 0 ? syscall6(SYS_accept, listener, 0, 0, 0, 0, 0) : -1;
	if (peer >= 0) {
		for (long more; n < (long)sizeof(got) - 1 && (more = syscall6(SYS_read, peer, (long)(got + n), sizeof(got) - 1 - n, 0, 0, 0)) > 0;) {
			n += more;
		}
		syscall6(SYS_close, peer, 0, 0, 0, 0, 0);
	}
	got[n] = '\0';
	if (pid > 0) {
		syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	}
	at = append(line, 0, sizeof(line), "unix-name: ");
	at = append(line, at, sizeof(line), argv[1]);
	if (syscall6(SYS_lstat64, (long)argv[1], (long)&st, 0, 0, 0, 0) == 0 && (st.mode & 0170000) == 0140000) {
		at = append(line, at, sizeof(line), " (a socket in the file system)");
	}
	at = append(line, at, sizeof(line), n > 0 ? ": a child connected and wrote: " : ": nothing came");
	at = append(line, at, sizeof(line), got);
	append(line, at, sizeof(line), "\n");
	say(line);
	syscall6(SYS_close, listener, 0, 0, 0, 0, 0);
	syscall6(SYS_unlink, (long)argv[1], 0, 0, 0, 0, 0);
}

/*
 * [NAME]: memory two processes share by a name (POSIX shared memory: shm_open, ftruncate, mmap MAP_SHARED):
 * a forked child writes into it, the parent reads what the child wrote, then the name is taken away
 */
void
command_shm(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "/qemu-init.shm";
	int in_child = 0;
	int32_t status = 0;
	long fd = syscall6(SYS_shm_open, (long)name, O_RDWR | O_CREAT_EXCL, 0600, 0, 0, 0), pid;
	volatile uint64_t *shared;
	char line[160];
	size_t at;

	if (fd < 0) {
		say("shm: ");
		say(name);
		say(": ");
		say_error(fd);
		say("\n");
		return;
	}
	if (syscall6(SYS_ftruncate, fd, 16384, 0, 0, 0, 0) != 0 ||
	    (long)(shared = (volatile uint64_t *)syscall6(SYS_mmap, 0, 16384, PROT_RW, MAP_SHARED, fd, 0)) < 0) {
		say("shm: not sized or not mapped\n");
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		syscall6(SYS_shm_unlink, (long)name, 0, 0, 0, 0, 0);
		return;
	}
	shared[0] = 1;
	shared[2047] = 1;
	pid = syscall_fork(&in_child);
	if (in_child) {
		shared[0] = 0x5a5a;             /* (into the same pages: the parent sees it) */
		shared[2047] = 0xa5a5;
		syscall6(SYS_exit, 0, 0, 0, 0, 0, 0);
	}
	if (pid < 0 || syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0) != pid) {
		say("shm: no child, or not waited for\n");
	} else {
		at = append(line, 0, sizeof(line), "shm: ");
		at = append(line, at, sizeof(line), name);
		at = append(line, at, sizeof(line), shared[0] == 0x5a5a && shared[2047] == 0xa5a5 ?
		    ": 16 KB shared with a child, its writes seen at both ends" : ": the child's writes are NOT seen");
		append(line, at, sizeof(line), "\n");
		say(line);
	}
	syscall6(SYS_munmap, (long)shared, 16384, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (syscall6(SYS_shm_unlink, (long)name, 0, 0, 0, 0, 0) != 0 ||
	    syscall6(SYS_shm_open, (long)name, O_RDWR, 0, 0, 0, 0) >= 0) {
		say("shm: the name was not taken away\n");
	}
}

#define SYS_select              93
#define SYS_poll                230
#define POLLIN                  0x0001

/*
 * poll and select on a pipe: empty, each waits out its timeout (100 ms) and says none ready; with bytes in it,
 * each says it is ready at once
 */
void
command_poll(int argc, char **argv)
{
	struct { int32_t fd; int16_t events, revents; } watched;
	struct { int64_t seconds; int32_t microseconds, pad; } tenth = { 0, 100000, 0 };
	uint32_t set[32];
	int ends[2];
	long empty_poll, empty_select, full_poll, full_select;
	int64_t poll_waited, select_waited;
	char line[240];
	size_t at;

	(void)argc;
	(void)argv;
	if (syscall_pipe(ends) != 0) {
		say("poll: no pipe\n");
		return;
	}
	watched.fd = ends[0];
	watched.events = POLLIN;
	watched.revents = 0;
	poll_waited = now_us();
	empty_poll = syscall6(SYS_poll, (long)&watched, 1, 100, 0, 0, 0);
	poll_waited = now_us() - poll_waited;
	for (int i = 0; i < 32; i++) {
		set[i] = 0;
	}
	set[ends[0] / 32] |= 1u << (ends[0] % 32);
	select_waited = now_us();
	empty_select = syscall6(SYS_select, ends[0] + 1, (long)set, 0, 0, (long)&tenth, 0);
	select_waited = now_us() - select_waited;
	syscall6(SYS_write, ends[1], (long)"x", 1, 0, 0, 0);
	watched.revents = 0;
	full_poll = syscall6(SYS_poll, (long)&watched, 1, 100, 0, 0, 0);
	for (int i = 0; i < 32; i++) {
		set[i] = 0;
	}
	set[ends[0] / 32] |= 1u << (ends[0] % 32);
	full_select = syscall6(SYS_select, ends[0] + 1, (long)set, 0, 0, (long)&tenth, 0);
	syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
	syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "poll: an empty pipe: poll ");
	at = append_number(line, at, sizeof(line), (uint64_t)empty_poll);
	at = append(line, at, sizeof(line), " ready after ");
	at = append_number(line, at, sizeof(line), (uint64_t)poll_waited / 1000);
	at = append(line, at, sizeof(line), " ms, select ");
	at = append_number(line, at, sizeof(line), (uint64_t)empty_select);
	at = append(line, at, sizeof(line), " after ");
	at = append_number(line, at, sizeof(line), (uint64_t)select_waited / 1000);
	at = append(line, at, sizeof(line), " ms; with a byte in it: poll ");
	at = append_number(line, at, sizeof(line), (uint64_t)full_poll);
	at = append(line, at, sizeof(line), (watched.revents & POLLIN) ? " (readable)" : " (NOT readable)");
	at = append(line, at, sizeof(line), ", select ");
	at = append_number(line, at, sizeof(line), (uint64_t)full_select);
	at = append(line, at, sizeof(line), "\n");
	syscall6(SYS_write, console, (long)line, (long)at, 0, 0, 0);
}

#define SYS_mkfifo              132
#define SYS_flock               131
#define O_RDONLY                0
#define LOCK_EX                 2
#define LOCK_NB                 4
#define LOCK_UN                 8

/* PATH: a named pipe made (mkfifo); a child opens it to write a line, this process to read it -- each open waits
 * for the other end's */
void
command_fifo(int argc, char **argv)
{
	char text[64], line[160];
	int in_child = 0;
	int32_t status = 0;
	long error, pid, fd, got;
	size_t at;

	if (argc != 2) {
		say("fifo PATH\n");
		return;
	}
	if ((error = syscall6(SYS_mkfifo, (long)argv[1], 0600, 0, 0, 0, 0)) != 0) {
		say_failed("fifo", argv[1], error);
		return;
	}
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("fifo: no child\n");
		return;
	}
	if (in_child) {
		fd = syscall6(SYS_open, (long)argv[1], O_WRONLY, 0, 0, 0, 0);
		syscall6(SYS_exit, fd >= 0 && syscall6(SYS_write, fd, (long)"written by a child", 18, 0, 0, 0) == 18 ? 0 : 1,
		    0, 0, 0, 0, 0);
	}
	fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0);
	at = 0;
	if (fd >= 0) {
		while (at < sizeof(text) - 1 && (got = syscall6(SYS_read, fd, (long)(text + at), (long)(sizeof(text) - 1 - at), 0, 0, 0)) > 0) {
			at += (size_t)got;      /* (to the end: the child's exit closes its end) */
		}
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	text[at] = '\0';
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "fifo: ");
	at = append(line, at, sizeof(line), argv[1]);
	if (fd < 0) {
		at = append(line, at, sizeof(line), ": not opened to read\n");
	} else {
		at = append(line, at, sizeof(line), ": read \"");
		at = append(line, at, sizeof(line), text);
		at = append(line, at, sizeof(line), status == 0 ? "\"\n" : "\"; the child's write failed\n");
	}
	say(line);
}

/* in a child: the file opened anew (a description of its own) and locked without waiting; its error, or 0 */
static long
lock_elsewhere(const char *path)
{
	int in_child = 0;
	int32_t status = 0;
	long pid = syscall_fork(&in_child), fd;

	if (pid < 0) {
		return pid;
	}
	if (in_child) {
		fd = syscall6(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
		syscall6(SYS_exit, fd < 0 ? 255 : -syscall6(SYS_flock, fd, LOCK_EX | LOCK_NB, 0, 0, 0, 0), 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	return -(long)((status >> 8) & 0xff);
}

/* FILE: the file locked (flock): another process's try, without waiting, refused; let go, another's taken */
void
command_flock(int argc, char **argv)
{
	char line[200];
	long fd, error, held, after;
	size_t at;

	if (argc != 2) {
		say("flock FILE\n");
		return;
	}
	if ((fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0)) < 0 ||
	    (error = syscall6(SYS_flock, fd, LOCK_EX, 0, 0, 0, 0)) != 0) {
		say_failed("flock", argv[1], fd < 0 ? fd : error);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	held = lock_elsewhere(argv[1]);
	syscall6(SYS_flock, fd, LOCK_UN, 0, 0, 0, 0);
	after = lock_elsewhere(argv[1]);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "flock: ");
	at = append(line, at, sizeof(line), argv[1]);
	at = append(line, at, sizeof(line), ": held, another's try ");
	at = append(line, at, sizeof(line), held == 0 ? "TAKEN" : "refused (error ");
	if (held != 0) {
		at = append_number(line, at, sizeof(line), (uint64_t)-held);
		at = append(line, at, sizeof(line), ")");
	}
	at = append(line, at, sizeof(line), "; let go, another's try ");
	at = append(line, at, sizeof(line), after == 0 ? "taken\n" : "REFUSED\n");
	say(line);
}

#define SYS_fcntl               92
#define F_GETLK                 7
#define F_SETLK                 8
#define F_RDLCK                 1
#define F_UNLCK                 2
#define F_WRLCK                 3

struct byte_lock {                      /* Darwin's struct flock */
	int64_t start, len;
	int32_t pid;
	int16_t type, whence;
};

static long
lock_bytes(long fd, int command, int16_t type, int64_t start, int64_t len, struct byte_lock *out)
{
	struct byte_lock l = { .start = start, .len = len, .pid = 0, .type = type, .whence = 0 /* SEEK_SET */ };
	long error = syscall6(SYS_fcntl, fd, command, (long)&l, 0, 0, 0);

	if (out != NULL) {
		*out = l;
	}
	return error;
}

/* FILE: bytes 0 to 9 of it locked to write (fcntl F_SETLK); in a child, a lock on 5 to 14 refused, on 10 to 19
 * taken, F_GETLK naming this process as what is in the way, and a lock to read 0 to 9 refused too; let go,
 * the child's lock on 0 to 9 taken */
void
command_byte_locks(int argc, char **argv)
{
	char line[256];
	struct byte_lock in_way;
	int in_child = 0;
	int32_t status = 0;
	long fd, error, pid, self = syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0);
	size_t at;
	int seen;

	if (argc != 2) {
		say("byte-locks FILE\n");
		return;
	}
	if ((fd = syscall6(SYS_open, (long)argv[1], O_RDWR, 0, 0, 0, 0)) < 0 ||
	    (error = lock_bytes(fd, F_SETLK, F_WRLCK, 0, 10, NULL)) != 0) {
		say_failed("byte-locks", argv[1], fd < 0 ? fd : error);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	if ((pid = syscall_fork(&in_child)) < 0) {
		say("byte-locks: no child\n");
		return;
	}
	if (in_child) {
		/* (a process's locks are its own: the child's descriptor, inherited or not, holds none of the parent's) */
		long mine = syscall6(SYS_open, (long)argv[1], O_RDWR, 0, 0, 0, 0);
		int bits = 0;

		bits |= lock_bytes(mine, F_SETLK, F_WRLCK, 5, 10, NULL) == -35 ? 1 : 0;
		bits |= lock_bytes(mine, F_SETLK, F_WRLCK, 10, 10, NULL) == 0 ? 2 : 0;
		bits |= lock_bytes(mine, F_GETLK, F_WRLCK, 0, 0, &in_way) == 0 && in_way.type == F_WRLCK &&
		    in_way.pid == self && in_way.start == 0 && in_way.len == 10 ? 4 : 0;
		bits |= lock_bytes(mine, F_SETLK, F_RDLCK, 0, 10, NULL) == -35 ? 8 : 0;
		syscall6(SYS_exit, bits, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	seen = (status >> 8) & 0xff;
	lock_bytes(fd, F_SETLK, F_UNLCK, 0, 10, NULL);
	if ((pid = syscall_fork(&in_child)) >= 0 && in_child) {
		long mine = syscall6(SYS_open, (long)argv[1], O_RDWR, 0, 0, 0, 0);

		syscall6(SYS_exit, lock_bytes(mine, F_SETLK, F_WRLCK, 0, 10, NULL) == 0 ? 16 : 0, 0, 0, 0, 0, 0);
	}
	if (pid >= 0) {
		syscall6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
		seen |= (status >> 8) & 0xff;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "byte-locks: ");
	at = append(line, at, sizeof(line), argv[1]);
	/* (short: a line of the screen is 140 characters) */
	at = append(line, at, sizeof(line), ": 0-9 held; another's 5-14 ");
	at = append(line, at, sizeof(line), seen & 1 ? "refused" : "NOT refused");
	at = append(line, at, sizeof(line), seen & 8 ? " (to read too)" : " (NOT to read)");
	at = append(line, at, sizeof(line), seen & 2 ? ", 10-19 taken" : ", 10-19 NOT taken");
	at = append(line, at, sizeof(line), seen & 4 ? ", F_GETLK names the holder" : ", F_GETLK does NOT name the holder");
	at = append(line, at, sizeof(line), seen & 16 ? "; let go, taken\n" : "; let go, NOT taken\n");
	say(line);
}

#define POLLOUT                 0x0004
#define F_SETFL                 4
#define O_NONBLOCK              4

/*
 * A pipe filled, a byte at a time without waiting, until the kernel says EAGAIN: how much it holds; full, poll
 * does not find it writable, and with some of it read out (PIPE_BUF, 512 bytes, the least a write is promised),
 * it does again
 */
void
command_pipe_size(int argc, char **argv)
{
	struct { int32_t fd; int16_t events, revents; } watched;
	static char out[1024];
	char line[200];
	int ends[2];
	long held = 0, n, last = 0, full, after;
	size_t at;

	(void)argc;
	(void)argv;
	if (syscall_pipe(ends) != 0 || syscall6(SYS_fcntl, ends[1], F_SETFL, O_NONBLOCK, 0, 0, 0) != 0) {
		say("pipe-size: no pipe\n");
		return;
	}
	while ((n = syscall6(SYS_write, ends[1], (long)"x", 1, 0, 0, 0)) == 1 && held < 1048576) {
		held++;
	}
	last = n;
	watched.fd = ends[1];
	watched.events = POLLOUT;
	watched.revents = 0;
	full = syscall6(SYS_poll, (long)&watched, 1, 0, 0, 0, 0);
	syscall6(SYS_read, ends[0], (long)out, 512, 0, 0, 0);
	watched.revents = 0;
	after = syscall6(SYS_poll, (long)&watched, 1, 0, 0, 0, 0) == 1 && (watched.revents & POLLOUT);
	syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
	syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "pipe-size: ");
	at = append_number(line, at, sizeof(line), (uint64_t)held);
	at = append(line, at, sizeof(line), last == -35 ? " bytes, then EAGAIN; full, " : " bytes, then NOT EAGAIN; full, ");
	at = append(line, at, sizeof(line), full == 0 ? "not writable" : "WRITABLE");
	at = append(line, at, sizeof(line), after ? "; 512 read out, writable again\n" : "; 512 read out, NOT writable again\n");
	say(line);
}

#define SOCK_DGRAM              2
#define SYS_recvfrom            29
#define SYS_sendto              133
#define SYS_shutdown            134
#define MSG_PEEK                0x2
#define MSG_TRUNC               0x10
#define MSG_NOSIGNAL            0x80000

/*
 * Datagrams through a socket pair (AF_UNIX, SOCK_DGRAM): three of 5, 300 and 1 bytes come out as three of
 * those sizes, in order; one looked at (MSG_PEEK) is still there; one longer than the room it is read into is
 * cut to it (Darwin's MSG_TRUNC says no more); shut down for sending, a send is refused (EPIPE, and with
 * "signal", SIGPIPE ends the process)
 */
void
command_dgram(int argc, char **argv)
{
	static char out[300], in[400];
	static const long sizes[] = { 5, 300, 1 };
	char line[240];
	int ends[2];
	long got[3], peek, again, cut, after;
	size_t at;

	for (unsigned i = 0; i < sizeof(out); i++) {
		out[i] = (char)('a' + i % 26);
	}
	if (syscall6(SYS_socketpair, AF_UNIX, SOCK_DGRAM, 0, (long)ends, 0, 0) != 0) {
		say("dgram: no socket pair\n");
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		syscall6(SYS_sendto, ends[0], (long)out, sizes[i], 0, 0, 0);
	}
	peek = syscall6(SYS_recvfrom, ends[1], (long)in, sizeof(in), MSG_PEEK, 0, 0);
	for (unsigned i = 0; i < 3; i++) {
		got[i] = syscall6(SYS_recvfrom, ends[1], (long)in, sizeof(in), 0, 0, 0);
	}
	syscall6(SYS_sendto, ends[0], (long)out, 300, 0, 0, 0);
	cut = syscall6(SYS_recvfrom, ends[1], (long)in, 100, MSG_TRUNC, 0, 0);
	syscall6(SYS_shutdown, ends[0], 1 /* SHUT_WR */, 0, 0, 0, 0);
	/* (MSG_NOSIGNAL: without it the send's SIGPIPE ends this process -- the first one, and the kernel with it;
	 * "dgram signal", in a child, leaves it out to see that) */
	after = syscall6(SYS_sendto, ends[0], (long)out, 1, argc > 1 && same(argv[1], "signal") ? 0 : MSG_NOSIGNAL, 0, 0);
	again = in[99] == out[99];
	syscall6(SYS_close, ends[0], 0, 0, 0, 0, 0);
	syscall6(SYS_close, ends[1], 0, 0, 0, 0, 0);
	at = append(line, 0, sizeof(line), "dgram: sent 5, 300, 1; looked at ");
	at = append_number(line, at, sizeof(line), (uint64_t)(peek < 0 ? 0 : peek));
	at = append(line, at, sizeof(line), ", then read ");
	for (unsigned i = 0; i < 3; i++) {
		at = append_number(line, at, sizeof(line), (uint64_t)(got[i] < 0 ? 0 : got[i]));
		at = append(line, at, sizeof(line), i < 2 ? ", " : "; 300 into 100: ");
	}
	at = append_number(line, at, sizeof(line), (uint64_t)(cut < 0 ? 0 : cut));
	at = append(line, at, sizeof(line), again ? " said (its first 100 as sent)" : " said (NOT as sent)");
	at = append(line, at, sizeof(line), after == -32 ? "; shut down, a send EPIPE\n" : "; shut down, a send NOT EPIPE\n");
	say(line);
}
