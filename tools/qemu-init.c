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
		say("mount TYPE DEVICE DIRECTORY, read only: mount apfs /dev/rvblk0 /mnt\n");
		return;
	}
	long error = syscall6(SYS_mount, (long)argv[1], (long)argv[3], MNT_RDONLY, (long)argv[2], 0, 0);
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
	{ "mount", "TYPE DEVICE DIR a disk's file system, read only", command_mount },
	{ "tone", "HZ [SECONDS] a sine wave on the sound device", command_tone },
	{ "input", "[COUNT] what the touch screen and the buttons say", command_input },
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
