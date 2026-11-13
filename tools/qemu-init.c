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

/* ---- the network ---- */

#define SYS_ioctl               54
#define SYS_socket              97
#define AF_INET                 2
#define AF_LINK                 18
#define SOCK_DGRAM              2
#define IFNAMSIZ                16
#define IFF_UP                  0x1
#define IFF_LOOPBACK            0x8
#define IFF_RUNNING             0x40
/* sys/sockio.h: _IOW/_IOWR('i', n, the structure) */
#define SIOCSIFFLAGS            0x80206910u     /* struct ifreq, 32 bytes */
#define SIOCGIFFLAGS            0xc0206911u
#define SIOCAIFADDR             0x8040691au     /* struct ifaliasreq, 64 bytes */
#define SIOCGIFCONF             0xc00c6924u     /* struct ifconf: 12 bytes, packed to 4 as net/if.h has it */

struct sockaddr_in {
	uint8_t         len, family;
	uint16_t        port;                   /* (network order) */
	uint8_t         addr[4];
	char            zero[8];
};

struct ifreq {
	char            name[IFNAMSIZ];
	union {
		struct sockaddr_in addr;
		int16_t         flags;
		char            pad[16];
	};
};

static void
say_ipv4(const uint8_t *a)
{
	for (int i = 0; i < 4; i++) {
		say_number(a[i]);
		say(i < 3 ? "." : "");
	}
}

static void
say_hex_byte(uint8_t b)
{
	char text[3] = { "0123456789abcdef"[b >> 4], "0123456789abcdef"[b & 0xf], '\0' };

	say(text);
}

/* "10.0.2.15" into four bytes; 0 when it is not an address */
static int
parse_ipv4(const char *text, uint8_t *out)
{
	for (int i = 0; i < 4; i++) {
		unsigned n = 0, digits = 0;

		while (*text >= '0' && *text <= '9' && digits < 4) {
			n = n * 10 + (unsigned)(*text++ - '0');
			digits++;
		}
		if (digits == 0 || n > 255 || (i < 3 && *text++ != '.')) {
			return 0;
		}
		out[i] = (uint8_t)n;
	}
	return *text == '\0';
}

static long
interface_flags(long sock, const char *name)
{
	struct ifreq request = { .name = { 0 } };

	for (int i = 0; i < IFNAMSIZ - 1 && name[i] != '\0'; i++) {
		request.name[i] = name[i];
	}
	return syscall6(SYS_ioctl, sock, (long)SIOCGIFFLAGS, (long)&request, 0, 0, 0) < 0 ? -1 : (uint16_t)request.flags;
}

static void
show_interfaces(long sock)
{
	char buffer[2048];
	struct __attribute__((packed, aligned(4))) { int32_t len; char *buf; } conf = { sizeof(buffer), buffer };
	char last[IFNAMSIZ] = "";

	if (syscall6(SYS_ioctl, sock, (long)SIOCGIFCONF, (long)&conf, 0, 0, 0) < 0) {
		say("ifconfig: the kernel will not list its interfaces\n");
		return;
	}
	/* an entry: the name, then a socket address of its own length (at least 16) */
	for (int at = 0; at + IFNAMSIZ + 2 <= conf.len;) {
		const char *name = buffer + at;
		const uint8_t *sa = (const uint8_t *)buffer + at + IFNAMSIZ;
		int sa_len = sa[0] > 16 ? sa[0] : 16;

		if (!same(name, last)) {
			long flags = interface_flags(sock, name);
			say(name);
			say(":");
			say(flags & IFF_UP ? " up" : " down");
			say(flags & IFF_RUNNING ? ", running" : "");
			say(flags & IFF_LOOPBACK ? ", loopback" : "");
			say("\n");
			for (int i = 0; i < IFNAMSIZ; i++) {
				last[i] = name[i];
			}
		}
		if (sa[1] == AF_INET) {
			say("\tinet ");
			say_ipv4(sa + 4);
			say("\n");
		} else if (sa[1] == AF_LINK && sa[6] == 6) {
			/* sockaddr_dl: len, family, index, type, name length, address length, selector length, name, address */
			say("\tether ");
			for (int i = 0; i < 6; i++) {
				say_hex_byte(sa[8 + sa[5] + i]);
				say(i < 5 ? ":" : "");
			}
			say("\n");
		}
		at += IFNAMSIZ + sa_len;
	}
}

/* NAME ADDRESS [MASK]: the address on the interface, and the interface up */
static void
configure_interface(long sock, char **argv, int argc)
{
	struct {
		char name[IFNAMSIZ];
		struct sockaddr_in addr, broadaddr, mask;
	} alias = { { 0 }, { 16, AF_INET, 0, { 0 }, { 0 } }, { 0 }, { 16, AF_INET, 0, { 255, 255, 255, 0 }, { 0 } } };
	struct ifreq request = { .name = { 0 } };

	if (!parse_ipv4(argv[2], alias.addr.addr) || (argc > 3 && !parse_ipv4(argv[3], alias.mask.addr))) {
		say("ifconfig: an address is four numbers: 10.0.2.15\n");
		return;
	}
	for (int i = 0; i < IFNAMSIZ - 1 && argv[1][i] != '\0'; i++) {
		alias.name[i] = request.name[i] = argv[1][i];
	}
	long flags = interface_flags(sock, argv[1]);
	request.flags = (int16_t)((flags < 0 ? 0 : flags) | IFF_UP);
	if (flags < 0 || syscall6(SYS_ioctl, sock, (long)SIOCSIFFLAGS, (long)&request, 0, 0, 0) < 0) {
		say("ifconfig: ");
		say(argv[1]);
		say(" is no interface here, or will not come up\n");
		return;
	}
	if (syscall6(SYS_ioctl, sock, (long)SIOCAIFADDR, (long)&alias, 0, 0, 0) < 0) {
		say("ifconfig: the address was not taken\n");
		return;
	}
	show_interfaces(sock);
}

static void
command_ifconfig(int argc, char **argv)
{
	long sock = syscall6(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);

	if (sock < 0) {
		say("ifconfig: no socket\n");
		return;
	}
	if (argc >= 3) {
		configure_interface(sock, argv, argc);
	} else {
		show_interfaces(sock);
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
}

/* ---- ping: ICMP echo on a datagram socket, which needs no privilege ---- */

#define SYS_recvfrom            29
#define SYS_sendto              133
#define SYS_setsockopt          105
#define IPPROTO_ICMP            1
#define SOL_SOCKET              0xffff
#define SO_RCVTIMEO             0x1006

static uint16_t
checksum(const uint8_t *data, size_t size)
{
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < size; i += 2) {
		sum += (uint32_t)(data[i] << 8 | data[i + 1]);
	}
	if (size & 1) {
		sum += (uint32_t)data[size - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static void
command_ping(int argc, char **argv)
{
	struct sockaddr_in to = { 16, AF_INET, 0, { 0 }, { 0 } }, from;
	struct { int64_t seconds; int32_t microseconds, pad; } wait = { 1, 0, 0 };
	int count = argc > 2 ? argv[2][0] - '0' : 3, answered = 0;

	if (argc < 2 || !parse_ipv4(argv[1], to.addr) || count < 1 || count > 9) {
		say("ping ADDRESS [COUNT 1-9]\n");
		return;
	}
	long sock = syscall6(SYS_socket, AF_INET, SOCK_DGRAM, IPPROTO_ICMP, 0, 0, 0);
	if (sock < 0) {
		say("ping: no ICMP socket\n");
		return;
	}
	syscall6(SYS_setsockopt, sock, SOL_SOCKET, SO_RCVTIMEO, (long)&wait, sizeof(wait), 0);
	for (int seq = 1; seq <= count; seq++) {
		uint8_t packet[64] = { 8, 0, 0, 0, 0x51, 0x55, 0, (uint8_t)seq }, reply[256];
		uint32_t size = sizeof(from);

		for (int i = 8; i < (int)sizeof(packet); i++) {
			packet[i] = (uint8_t)i;
		}
		uint16_t sum = checksum(packet, sizeof(packet));
		packet[2] = (uint8_t)(sum >> 8);
		packet[3] = (uint8_t)sum;
		if (syscall6(SYS_sendto, sock, (long)packet, sizeof(packet), 0, (long)&to, sizeof(to)) < 0) {
			say("ping: not sent (no route to it?)\n");
			break;
		}
		long got = syscall6(SYS_recvfrom, sock, (long)reply, sizeof(reply), 0, (long)&from, (long)&size);
		/* what comes back has the IP header before the ICMP message */
		long ip = got > 0 && reply[0] >> 4 == 4 ? (reply[0] & 0xf) * 4 : 0;
		if (got >= ip + 8 && reply[ip] == 0 && reply[ip + 7] == seq) {
			say_number((uint64_t)(got - ip));
			say(" bytes from ");
			say_ipv4(from.addr);
			say(": icmp_seq=");
			say_number((uint64_t)seq);
			say("\n");
			answered++;
		} else {
			say("no answer to icmp_seq=");
			say_number((uint64_t)seq);
			say("\n");
		}
	}
	say_number((uint64_t)count);
	say(" sent, ");
	say_number((uint64_t)answered);
	say(" answered\n");
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
}

/* ---- serve: a page over HTTP, the kernel's TCP from listen to close ---- */

#define SYS_accept              30
#define SYS_bind                104
#define SYS_listen              106
#define SOCK_STREAM             1
#define SO_REUSEADDR            0x4

static void
put(long fd, const char *s)
{
	syscall6(SYS_write, fd, (long)s, (long)length(s), 0, 0, 0);
}

static void
put_number(long fd, uint64_t n)
{
	char digits[24];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	put(fd, &digits[i]);
}

/* the page: what the kernel says of itself, as text */
static void
page(long fd)
{
	char text[256];
	uint64_t n;

	put(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
	put(fd, "Served by /sbin/launchd (tools/qemu-init.c) on XNU, through the kernel's TCP and its virtio network driver.\n");
	if (sysctl_string("kern.version", text, sizeof(text))) {
		put(fd, text);
		put(fd, "\n");
	}
	if (sysctl_string("hw.board.name", text, sizeof(text))) {
		put(fd, "board: ");
		put(fd, text);
		put(fd, "\n");
	}
	if (sysctl_number("hw.memsize", &n)) {
		put(fd, "memory: ");
		put_number(fd, n >> 20);
		put(fd, " MB\n");
	}
}

static void
command_serve(int argc, char **argv)
{
	struct sockaddr_in here = { 16, AF_INET, 0x5000, { 0 }, { 0 } };     /* port 80, any address */
	int count = argc > 1 ? argv[1][0] - '0' : 1, yes = 1;

	if (count < 1 || count > 9) {
		say("serve [COUNT 1-9]\n");
		return;
	}
	long sock = syscall6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
	if (sock < 0) {
		say("serve: no socket\n");
		return;
	}
	syscall6(SYS_setsockopt, sock, SOL_SOCKET, SO_REUSEADDR, (long)&yes, sizeof(yes), 0);
	if (syscall6(SYS_bind, sock, (long)&here, sizeof(here), 0, 0, 0) < 0 || syscall6(SYS_listen, sock, 4, 0, 0, 0, 0) < 0) {
		say("serve: port 80 cannot be had\n");
		syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
		return;
	}
	say("serving on port 80: ");
	say_number((uint64_t)count);
	say(count > 1 ? " requests\n" : " request\n");
	for (int served = 0; served < count; served++) {
		struct sockaddr_in from;
		uint32_t size = sizeof(from);
		char request[512];
		long peer = syscall6(SYS_accept, sock, (long)&from, (long)&size, 0, 0, 0);

		if (peer < 0) {
			break;
		}
		long got = syscall6(SYS_read, peer, (long)request, sizeof(request) - 1, 0, 0, 0);
		request[got > 0 ? got : 0] = '\0';
		for (long i = 0; i < got; i++) {
			if (request[i] == '\r' || request[i] == '\n') {
				request[i] = '\0';
				break;
			}
		}
		say("  from ");
		say_ipv4(from.addr);
		say(": ");
		say(request);
		say("\n");
		page(peer);
		syscall6(SYS_close, peer, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
}

/* ---- mount ---- */

#define SYS_mount               167
#define MNT_RDONLY              0x1

/* TYPE DEVICE DIRECTORY, read only: what the file system takes as its data is its own (APFS: the device) */
static void
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

/* ---- tone: a sine wave to the kernel's sound device ---- */

#define O_WRONLY                1
#define AUDIO_RATE              48000           /* sys/board/audio.h: 48 kHz, two channels of 16 bits */

static unsigned
parse_number(const char *text)
{
	unsigned n = 0;

	while (*text >= '0' && *text <= '9') {
		n = n * 10 + (unsigned)(*text++ - '0');
	}
	return *text == '\0' ? n : 0;
}

#define SYS_select              93

/* microseconds since 1970 */
static int64_t
now_us(void)
{
	int64_t tv[2] = { 0, 0 };       /* struct timeval: seconds, then microseconds in 32 bits */

	syscall6(SYS_gettimeofday, (long)tv, 0, 0, 0, 0, 0);
	return tv[0] * 1000000 + (int32_t)tv[1];
}

static void
sleep_us(int64_t us)
{
	int64_t tv[2] = { us / 1000000, us % 1000000 };

	syscall6(SYS_select, 0, 0, 0, 0, (long)tv, 0);
}

/*
 * HZ [SECONDS]: a sine of HZ (y[n] = 2 cos(w) y[n-1] - y[n-2], no libm), a tenth of a second at a time
 * and in time: the device takes what it is given at once and plays it at its own pace, holding no more
 * than a third of a second, so the writes keep two tenths ahead of the clock.
 */
static void
command_tone(int argc, char **argv)
{
	unsigned hz = argc > 1 ? parse_number(argv[1]) : 0, seconds = argc > 2 ? parse_number(argv[2]) : 1;
	static int16_t frames[AUDIO_RATE / 10 * 2];

	if (hz < 20 || hz > 20000 || seconds < 1 || seconds > 30) {
		say("tone HZ [SECONDS]: 20 to 20000 Hz, 1 to 30 s\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)"/dev/hakios-audio", O_WRONLY, 0, 0, 0, 0);
	if (fd < 0) {
		say("tone: no sound device (/dev/hakios-audio)\n");
		return;
	}
	double w = 2 * 3.14159265358979 * hz / AUDIO_RATE, w2 = w * w;
	double c = 1 - w2 / 2 * (1 - w2 / 12 * (1 - w2 / 30 * (1 - w2 / 56)));        /* cos(w) */
	double s1 = 0, s2 = -8000 * (w - w * w2 / 6 * (1 - w2 / 20));                   /* sin(-w) times the level */

	int64_t start = now_us();

	for (unsigned tenth = 0; tenth < seconds * 10; tenth++) {
		int64_t early = start + ((int64_t)tenth - 2) * 100000 - now_us();

		if (early > 0) {
			sleep_us(early);
		}
		for (unsigned i = 0; i < AUDIO_RATE / 10; i++) {
			double y = 2 * c * s1 - s2;

			s2 = s1;
			s1 = y;
			frames[2 * i] = frames[2 * i + 1] = (int16_t)y;
		}
		if (syscall6(SYS_write, fd, (long)frames, sizeof(frames), 0, 0, 0) != (long)sizeof(frames)) {
			say("tone: the sound device took less than it was given\n");
			break;
		}
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("played ");
	say_number(hz);
	say(" Hz for ");
	say_number(seconds);
	say(" s\n");
}

/* ---- input: what the touch screen and the buttons say (sys/board/input.h) ---- */

struct touch_record {
	uint64_t time;
	uint32_t type, finger, x, y;
};

/* [COUNT]: the next COUNT records of /dev/hakios-touch, as they come */
static void
command_input(int argc, char **argv)
{
	static const char *const types[] = { "?", "touch down", "touch move", "touch up", "button down", "button up" };
	static const char *const buttons[] = { "?", "lock", "volume up", "volume down", "home" };
	unsigned count = argc > 1 ? parse_number(argv[1]) : 4;
	struct touch_record r;

	if (count < 1 || count > 1000) {
		say("input [COUNT]: 1 to 1000 records\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)"/dev/hakios-touch", 0, 0, 0, 0, 0);
	if (fd < 0) {
		say("input: no input device (/dev/hakios-touch)\n");
		return;
	}
	say("input: waiting for ");
	say_number(count);
	say(" records\n");
	for (unsigned i = 0; i < count && syscall6(SYS_read, fd, (long)&r, sizeof(r), 0, 0, 0) == (long)sizeof(r); i++) {
		say(r.type < 6 ? types[r.type] : "?");
		if (r.type >= 4) {
			say(" ");
			say(r.finger < 5 ? buttons[r.finger] : "?");
		} else {
			say(" finger ");
			say_number(r.finger);
			say(" at ");
			say_number(r.x);
			say(",");
			say_number(r.y);
		}
		say("\n");
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}

/* ---- dmesg and run: the kernel's messages, and a process of one's own ---- */

#define PROC_INFO_CALL_KERNMSGBUF 4
#define SYS_wait4               7
#define SYS_posix_spawn         244

/* [LINES]: the last LINES lines (20 if not said) of the kernel's message buffer */
static void
command_dmesg(int argc, char **argv)
{
	static char text[128 * 1024];
	unsigned lines = argc > 1 ? parse_number(argv[1]) : 20;

	long got = syscall6(SYS_proc_info, PROC_INFO_CALL_KERNMSGBUF, 0, 0, 0, (long)text, sizeof(text));
	if (got <= 0 || lines == 0) {
		say(got <= 0 ? "dmesg: the kernel will not give its messages\n" : "dmesg [LINES]: a number of lines\n");
		return;
	}
	while (got > 0 && (text[got - 1] == '\0' || text[got - 1] == '\n')) {
		got--;
	}
	text[got] = '\0';
	long from = got;
	for (unsigned seen = 0; from > 0; from--) {
		if (text[from - 1] == '\n' && ++seen == lines) {
			break;
		}
	}
	say(text + from);
	say("\n");
}

/* how a process ended, as wait4 says it */
static void
say_ended(int32_t pid, int32_t status)
{
	say("pid ");
	say_number((uint64_t)pid);
	if ((status & 0x7f) == 0) {
		say(" exited with ");
		say_number((uint64_t)((status >> 8) & 0xff));
	} else {
		say(" ended by signal ");
		say_number((uint64_t)(status & 0x7f));
	}
	say("\n");
}

/* PROGRAM [ARGUMENTS]: a new process (posix_spawn) running PROGRAM, waited for; what it ended with */
static void
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
	say("run: ");
	say_ended(pid, status);
}

/* ---- mach: a message to itself through a port of its own ---- */

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

static long
trap8(long number, long a, long b, long c, long d, long e, long f, long g, long h)
{
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x7 __asm__("x7") = h;
	register long x16 __asm__("x16") = number;

	__asm__ volatile ("svc #0x80"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
	    : "r"(x16)
	    : "memory", "cc");
	return x0;
}

struct mach_message {
	uint32_t bits, size, remote_port, local_port, voucher_port;
	int32_t id;
	uint64_t number;
	uint8_t trailer[200];           /* what the kernel adds on receiving */
};

/* [COUNT]: COUNT messages (1000 if not said) sent to a port and received from it, and the time each took */
static void
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

/* ---- date, sleep, and processes in the background: spawn, kill, wait ---- */

#define SYS_kill                37
#define SYS_pipe                42

/* the date and time, UTC, from the seconds since 1970 (the civil calendar's days, without a library) */
static void
command_date(int argc, char **argv)
{
	int64_t seconds = now_us() / 1000000, days = seconds / 86400, of_day = seconds % 86400;
	(void)argc;
	(void)argv;

	/* days since 1970-01-01 to a year, month and day (H. Hinnant's days_from_civil, backwards) */
	int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
	int64_t year = yoe + era * 400 + (month <= 2);
	const int64_t parts[] = { year, month, day, of_day / 3600, of_day / 60 % 60, of_day % 60 };
	const char *const after[] = { "-", "-", " ", ":", ":", " UTC\n" };

	for (int i = 0; i < 6; i++) {
		if (i > 0 && parts[i] < 10) {
			say("0");
		}
		say_number((uint64_t)parts[i]);
		say(after[i]);
	}
}

/* SECONDS: nothing, for that long */
static void
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
static void
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
static void
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
static void
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
	say("wait: ");
	say_ended((int32_t)pid, status);
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
static void
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

/* ---- files written: mkdir, write, rm ---- */

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
static void
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
static void
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
static void
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

/* ---- threads: work on every processor ---- */

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

/* COUNT threads doing ROUNDS each; how long, against one doing all of it alone; -1 if they did not all end */
static int64_t
threads_run(unsigned count, uint32_t rounds)
{
	static uint8_t stacks[THREADS_MAX][THREAD_STACK] __attribute__((aligned(16)));
	static uint8_t memory[THREADS_MAX][4096] __attribute__((aligned(16)));
	int64_t start = now_us();

	threads_total = 0;
	threads_done = 0;
	thread_rounds = rounds;
	for (unsigned i = 0; i < count; i++) {
		if (syscall6(SYS_bsdthread_create, (long)thread_work, 0, (long)(stacks[i] + THREAD_STACK), (long)memory[i],
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
static void
command_threads(int argc, char **argv)
{
	static int registered;
	uint32_t processors = 1;
	size_t size = sizeof(processors);
	const uint32_t work = 1u << 26;

	syscall6(SYS_sysctlbyname, (long)"hw.ncpu", 7, (long)&processors, (long)&size, 0, 0);
	unsigned count = argc > 1 ? parse_number(argv[1]) : processors;
	if (count < 1 || count > THREADS_MAX) {
		say("threads [COUNT]: 1 to 8\n");
		return;
	}
	if (!registered && syscall6(SYS_bsdthread_register, (long)thread_entry, 0, 0, 0, 0, 0) < 0) {    /* (it says its features) */
		say("threads: the kernel takes no threads from this process\n");
		return;
	}
	registered = 1;
	int64_t alone = threads_run(1, work), shared = threads_run(count, work / count);
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

/* ---- mmap and kevent: a file's pages, and events waited for ---- */

#define SYS_munmap              73
#define SYS_mmap                197
#define SYS_fstat64             339
#define SYS_kqueue              362
#define SYS_kevent              363
#define PROT_READ               1
#define MAP_PRIVATE             2
#define EVFILT_READ             (-1)
#define EVFILT_TIMER            (-7)
#define EV_ADD                  1

/* FNV-1a, over bytes, from where it got to */
static uint64_t
fnv(uint64_t hash, const uint8_t *bytes, long n)
{
	for (long i = 0; i < n; i++) {
		hash = (hash ^ bytes[i]) * 0x100000001b3ull;
	}
	return hash;
}

/* FILE: the file mapped (its pages come from its file system), checked against what read() gives */
static void
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

struct kevent64 {                       /* bsd/sys/event.h's struct kevent, packed to 4 */
	uint64_t ident;
	int16_t filter;
	uint16_t flags;
	uint32_t fflags;
	int64_t data;
	uint64_t udata;
} __attribute__((packed, aligned(4)));

/* [COUNT]: a timer of 50 ms through kqueue, COUNT times (10 if not said), then a pipe seen to be readable */
static void
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

/* ---- the network the other way: get (a TCP client) and udp ---- */

#define SYS_connect             98

/* ADDRESS PORT: a socket address, or 0 */
static int
parse_endpoint(char **argv, struct sockaddr_in *to)
{
	unsigned port = parse_number(argv[2]);

	to->len = 16;
	to->family = AF_INET;
	to->port = (uint16_t)((port >> 8) | (port << 8));
	return parse_ipv4(argv[1], to->addr) && port > 0 && port < 65536;
}

/* ADDRESS PORT [PATH]: a page asked for over HTTP (connect, as a client); its first line and its size */
static void
command_get(int argc, char **argv)
{
	static char response[16384];
	struct sockaddr_in to = { 0 };
	struct { int64_t seconds; int32_t microseconds, pad; } wait = { 10, 0, 0 };
	const char *path = argc > 3 ? argv[3] : "/";

	if (argc < 3 || !parse_endpoint(argv, &to)) {
		say("get ADDRESS PORT [PATH]: get 10.0.2.2 8000 /\n");
		return;
	}
	long sock = syscall6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
	if (sock < 0) {
		say("get: no socket\n");
		return;
	}
	syscall6(SYS_setsockopt, sock, SOL_SOCKET, SO_RCVTIMEO, (long)&wait, sizeof(wait), 0);
	long error = syscall6(SYS_connect, sock, (long)&to, sizeof(to), 0, 0, 0);
	if (error != 0) {
		say("get: no connection, error ");
		say_number((uint64_t)-error);
		say("\n");
		syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
		return;
	}
	const char *const request[] = { "GET ", path, " HTTP/1.0\r\nHost: ", argv[1], "\r\nUser-Agent: qemu-init\r\n\r\n" };
	for (int i = 0; i < 5; i++) {
		syscall6(SYS_write, sock, (long)request[i], (long)length(request[i]), 0, 0, 0);
	}
	long all = 0;
	for (long got; all < (long)sizeof(response) - 1 &&
	    (got = syscall6(SYS_read, sock, (long)response + all, (long)sizeof(response) - 1 - all, 0, 0, 0)) > 0; all += got) {
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
	response[all] = '\0';
	long line = 0, body = 0;
	while (line < all && response[line] != '\r' && response[line] != '\n') {
		line++;
	}
	for (long i = 0; i + 3 < all; i++) {
		if (response[i] == '\r' && response[i + 1] == '\n' && response[i + 2] == '\r' && response[i + 3] == '\n') {
			body = i + 4;
			break;
		}
	}
	response[line] = '\0';
	say("get: ");
	say(response);
	say(", ");
	say_number((uint64_t)(body > 0 ? all - body : 0));
	say(" bytes of body\n");
	if (body > 0) {
		say(response + body);
		if (all > body && response[all - 1] != '\n') {
			say("\n");
		}
	}
}

/* ADDRESS PORT WORDS...: the words in a datagram, and what comes back */
static void
command_udp(int argc, char **argv)
{
	struct sockaddr_in to = { 0 }, from;
	struct { int64_t seconds; int32_t microseconds, pad; } wait = { 5, 0, 0 };
	char text[512], back[512];
	long n = 0;

	if (argc < 4 || !parse_endpoint(argv, &to)) {
		say("udp ADDRESS PORT WORDS...\n");
		return;
	}
	for (int i = 3; i < argc; i++) {
		for (const char *c = argv[i]; *c != '\0' && n < (long)sizeof(text) - 1; c++) {
			text[n++] = *c;
		}
		if (i + 1 < argc && n < (long)sizeof(text) - 1) {
			text[n++] = ' ';
		}
	}
	long sock = syscall6(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
	if (sock < 0) {
		say("udp: no socket\n");
		return;
	}
	syscall6(SYS_setsockopt, sock, SOL_SOCKET, SO_RCVTIMEO, (long)&wait, sizeof(wait), 0);
	uint32_t size = sizeof(from);
	long got = -1;
	if (syscall6(SYS_sendto, sock, (long)text, n, 0, (long)&to, sizeof(to)) == n) {
		got = syscall6(SYS_recvfrom, sock, (long)back, sizeof(back) - 1, 0, (long)&from, (long)&size);
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
	if (got < 0) {
		say("udp: nothing came back\n");
		return;
	}
	back[got] = '\0';
	say("udp: ");
	say_number((uint64_t)got);
	say(" bytes back from ");
	say_ipv4(from.addr);
	say(": ");
	say(back);
	say("\n");
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
	{ "ifconfig", "[NAME ADDRESS [MASK]] the interfaces, or an address for one", command_ifconfig },
	{ "ping", "ADDRESS [COUNT] ICMP echo: is it there", command_ping },
	{ "serve", "[COUNT] a page over HTTP on port 80", command_serve },
	{ "get", "ADDRESS PORT [PATH] a page asked for over HTTP", command_get },
	{ "udp", "ADDRESS PORT WORDS... a datagram, and its answer", command_udp },
	{ "mount", "TYPE DEVICE DIR a disk's file system (read only), or ramfs", command_mount },
	{ "tone", "HZ [SECONDS] a sine wave on the sound device", command_tone },
	{ "input", "[COUNT] what the touch screen and the buttons say", command_input },
	{ "dmesg", "[LINES] the kernel's latest messages", command_dmesg },
	{ "run", "PROGRAM [ARGUMENTS] a process of its own, waited for", command_run },
	{ "mach", "[COUNT] messages to itself through a Mach port, timed", command_mach },
	{ "date", "the date and time, UTC", command_date },
	{ "sleep", "SECONDS nothing, for that long", command_sleep },
	{ "spawn", "PROGRAM [ARGUMENTS] a process of its own, not waited for", command_spawn },
	{ "kill", "PID [SIGNAL] a signal to a process (PID !: the one spawned last)", command_kill },
	{ "wait", "the next of its processes to end", command_wait },
	{ "pipe", "WORDS... the words through a pipe", command_pipe },
	{ "mkdir", "DIR... new directories", command_mkdir },
	{ "write", "FILE WORDS... the words as all of a file", command_write },
	{ "rm", "PATH... files, or empty directories, gone", command_rm },
	{ "threads", "[COUNT] threads sharing some work, timed against one", command_threads },
	{ "mmap", "FILE the file's pages, checked against its reads", command_mmap },
	{ "kevent", "[COUNT] a timer and a pipe, waited for through kqueue", command_kevent },
};

static void
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
