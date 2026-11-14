/* tools/qemu-init: the network: ifconfig, ping, serve, get, udp */
#include "init.h"

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

void
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

void
command_ping(int argc, char **argv)
{
	struct sockaddr_in to = { 16, AF_INET, 0, { 0 }, { 0 } }, from;
	struct { int64_t seconds; int32_t microseconds, pad; } wait = { 1, 0, 0 };
	int count = argc > 2 ? (int)parse_number(argv[2]) : 3, answered = 0;

	if (argc < 2 || !parse_ipv4(argv[1], to.addr) || count < 1 || count > 100) {
		say("ping ADDRESS [COUNT]: 1 to 100 echoes\n");
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

/*
 * GET /bytes/N: N bytes made up as they go (byte i is i * 7 mod 251, which the asker can make too), for
 * the kernel's TCP to send a lot of; at most 64 MB
 */
static int
bytes_page(long fd, const char *request)
{
	static uint8_t chunk[65536];
	const char *prefix = "GET /bytes/";
	uint64_t total = 0, sent = 0;

	for (int i = 0; prefix[i] != '\0'; i++) {
		if (request[i] != prefix[i]) {
			return 0;
		}
	}
	for (const char *c = request + length(prefix); *c >= '0' && *c <= '9'; c++) {
		total = total * 10 + (uint64_t)(*c - '0');
	}
	if (total > (64u << 20)) {
		total = 64u << 20;
	}
	put(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
	put_number(fd, total);
	put(fd, "\r\nConnection: close\r\n\r\n");
	while (sent < total) {
		uint64_t n = total - sent < sizeof(chunk) ? total - sent : sizeof(chunk);

		for (uint64_t i = 0; i < n; i++) {
			chunk[i] = (uint8_t)((sent + i) * 7 % 251);
		}
		if (syscall6(SYS_write, fd, (long)chunk, (long)n, 0, 0, 0) != (long)n) {
			break;
		}
		sent += n;
	}
	return 1;
}

/* the page: what the kernel says of itself, as text */
static void
page(long fd)
{
	char text[256];
	uint64_t n;

	put(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
	put(fd, "Served by /sbin/launchd (tools/qemu-init) on XNU, through the kernel's TCP and its virtio network driver.\n");
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

void
command_serve(int argc, char **argv)
{
	struct sockaddr_in here = { 16, AF_INET, 0x5000, { 0 }, { 0 } };     /* port 80, any address */
	int count = argc > 1 ? (int)parse_number(argv[1]) : 1, yes = 1;

	if (count < 1 || count > 100) {
		say("serve [COUNT]: 1 to 100 requests\n");
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
	{
		/* (in one write: run from spawn, a line said in pieces may have the shell's prompt put in its middle) */
		char line[64];
		size_t at = append(line, 0, sizeof(line), "serving on port 80: ");

		at = append_number(line, at, sizeof(line), (uint64_t)count);
		at = append(line, at, sizeof(line), count > 1 ? " requests\n" : " request\n");
		say(line);
	}
	for (int served = 0; served < count; served++) {
		struct sockaddr_in from;
		uint32_t size = sizeof(from);
		char request[512];
		long peer = syscall6(SYS_accept, sock, (long)&from, (long)&size, 0, 0, 0);

		if (peer < 0) {
			break;
		}
		/* (the request read to the end of its headers: TCP may bring it in several pieces) */
		long got = 0;
		for (long n; got < (long)sizeof(request) - 1 &&
		    (n = syscall6(SYS_read, peer, (long)(request + got), sizeof(request) - 1 - got, 0, 0, 0)) > 0;) {
			got += n;
			request[got] = '\0';
			if (got >= 4 && same(request + got - 4, "\r\n\r\n")) {
				break;
			}
		}
		request[got] = '\0';
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
		if (!bytes_page(peer, request)) {
			page(peer);
		}
		syscall6(SYS_close, peer, 0, 0, 0, 0, 0);
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
}

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

/* ADDRESS PORT [PATH]: a page asked for over HTTP (connect, as a client): its first line, its size, the time it
 * took and its hash; and the page itself if it is short */
void
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
		say("get: no connection, ");
		say_error(error);
		say("\n");
		syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
		return;
	}
	/* (the request in one write) */
	char request[640];
	size_t at = append(request, 0, sizeof(request), "GET ");
	at = append(request, at, sizeof(request), path);
	at = append(request, at, sizeof(request), " HTTP/1.0\r\nHost: ");
	at = append(request, at, sizeof(request), argv[1]);
	at = append(request, at, sizeof(request), "\r\nUser-Agent: qemu-init\r\n\r\n");
	syscall6(SYS_write, sock, (long)request, (long)at, 0, 0, 0);
	/*
	 * The first 16 KB kept (the head, and a small body to show); all of it counted, timed and hashed
	 * (FNV-1a over the body, for the other side to compare), however long it is.
	 */
	long all = 0, kept = 0, body = -1;
	uint64_t hash = FNV1A_START;
	int64_t start = now_us();
	static char chunk[65536];
	for (long got; (got = syscall6(SYS_read, sock, (long)chunk, sizeof(chunk), 0, 0, 0)) > 0; all += got) {
		for (long i = 0; i < got; i++) {
			if (kept < (long)sizeof(response) - 1) {
				response[kept++] = chunk[i];
			}
			if (body >= 0) {
				hash = fnv1a(hash, chunk + i, 1);
			} else if (all + i >= 3 && kept == all + i + 1 && response[all + i - 3] == '\r' && response[all + i - 2] == '\n' &&
			    response[all + i - 1] == '\r' && response[all + i] == '\n') {
				body = all + i + 1;     /* (the head ends here; what follows is the body) */
			}
		}
	}
	int64_t took = now_us() - start;
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
	response[kept] = '\0';
	long line = 0;
	while (line < kept && response[line] != '\r' && response[line] != '\n') {
		line++;
	}
	long body_size = body >= 0 ? all - body : 0;
	response[line] = '\0';
	say("get: ");
	say(response);
	say(", ");
	say_number((uint64_t)body_size);
	say(" bytes of body in ");
	say_number((uint64_t)took / 1000);
	say(" ms, fnv1a ");
	for (int shift = 60; shift >= 0; shift -= 4) {
		char digit[2] = { "0123456789abcdef"[(hash >> shift) & 15], '\0' };
		say(digit);
	}
	say("\n");
	if (body >= 0 && body_size > 0 && body_size <= 1024 && all <= kept) {
		say(response + body);
		if (response[kept - 1] != '\n') {
			say("\n");
		}
	}
}

/* ADDRESS PORT WORDS...: the words in a datagram, and what comes back */
void
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

/* PORT [COUNT]: datagrams on that UDP port sent back to whoever sent them, COUNT of them (1 if not said) */
void
command_echo_udp(int argc, char **argv)
{
	unsigned port = argc > 1 ? parse_number(argv[1]) : 0, count = argc > 2 ? parse_number(argv[2]) : 1;
	struct sockaddr_in here = { 16, AF_INET, (uint16_t)(port >> 8 | (port & 0xff) << 8), { 0 }, { 0 } }, from;
	char datagram[1500];

	if (port < 1 || port > 65535 || count < 1 || count > 100) {
		say("echo-udp PORT [COUNT]: a port, 1 to 100 datagrams\n");
		return;
	}
	long sock = syscall6(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
	if (sock < 0 || syscall6(SYS_bind, sock, (long)&here, sizeof(here), 0, 0, 0) < 0) {
		say("echo-udp: the port cannot be had\n");
		if (sock >= 0) {
			syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
		}
		return;
	}
	for (unsigned n = 0; n < count; n++) {
		uint32_t size = sizeof(from);
		long got = syscall6(SYS_recvfrom, sock, (long)datagram, sizeof(datagram), 0, (long)&from, (long)&size);

		if (got < 0 || syscall6(SYS_sendto, sock, (long)datagram, got, 0, (long)&from, size) != got) {
			say("echo-udp: a datagram lost\n");
			break;
		}
	}
	syscall6(SYS_close, sock, 0, 0, 0, 0, 0);
}
