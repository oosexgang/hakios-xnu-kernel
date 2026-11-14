/* tools/qemu-init: the kernel's values, messages and time: sysctl, vm, uptime, dmesg, date, random */
#include "init.h"

/*
 * NAME[=VALUE]...: a value as the kernel types it, and with =VALUE set first: its name to its numbers
 * (sysctl 0.3, name2oid), their kind and format
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

static void sysctl_one(char *name);

/* NAME is PREFIX, or under it (PREFIX.something); every name is under the empty one */
static int
under_prefix(const char *name, const char *prefix, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (name[i] != prefix[i]) {
			return 0;
		}
	}
	return name[n] == '\0' || name[n] == '.' || n == 0;
}

/*
 * -a [PREFIX]: every name the kernel has (sysctl 0.2, next: from one leaf to the one after; 0.1, the name of
 * one), and the value of each under PREFIX; how many there were
 */
static void
sysctl_all(const char *prefix)
{
	int next[CTL_MAXNAME + 2] = { 0, 2 }, name_of[CTL_MAXNAME + 2] = { 0, 1 }, oid[CTL_MAXNAME];
	unsigned depth = 0, all = 0, under = 0;
	size_t prefix_length = length(prefix);
	char name[256];

	for (;;) {
		size_t size = sizeof(oid);

		for (unsigned i = 0; i < depth; i++) {
			next[2 + i] = oid[i];
		}
		if (syscall6(SYS___sysctl, (long)next, depth + 2, (long)oid, (long)&size, 0, 0) < 0 || size == 0) {
			break;
		}
		depth = (unsigned)(size / sizeof(int));
		for (unsigned i = 0; i < depth; i++) {
			name_of[2 + i] = oid[i];
		}
		size = sizeof(name) - 1;
		if (syscall6(SYS___sysctl, (long)name_of, depth + 2, (long)name, (long)&size, 0, 0) < 0) {
			continue;
		}
		name[size] = '\0';
		if (under_prefix(name, "sysctl", 6)) {
			continue;               /* (sysctl's own: reading sysctl.debug prints the whole tree on the console) */
		}
		all++;
		if (!under_prefix(name, prefix, prefix_length)) {
			continue;
		}
		under++;
		sysctl_one(name);
	}
	say("sysctl: ");
	say_number(under);
	say(prefix_length ? " names under " : " names");
	say(prefix_length ? prefix : "");
	say(", of the ");
	say_number(all);
	say(" the kernel has\n");
}

void
command_sysctl(int argc, char **argv)
{
	if (argc > 1 && same(argv[1], "-a")) {
		sysctl_all(argc > 2 ? argv[2] : "");
		return;
	}
	for (int i = 1; i < argc; i++) {
		sysctl_one(argv[i]);
	}
}

/* NAME[=VALUE] */
static void
sysctl_one(char *name)
{
	int query[2] = { 0, 3 }, mib[CTL_MAXNAME + 2] = { 0, 4 };
	size_t size = CTL_MAXNAME * sizeof(int);
	union { uint32_t kind; char bytes[64]; } format;
	union { char text[512]; int32_t i32[128]; int64_t i64[64]; } value;

	/* NAME=VALUE: the value set first (a string, or a number for a number) */
	char *setting = name;
	while (*setting != '\0' && *setting != '=') {
		setting++;
	}
	if (*setting == '=') {
		*setting++ = '\0';
	} else {
		setting = NULL;
	}
	say(name);
	say(": ");
	if (syscall6(SYS___sysctl, (long)query, 2, (long)&mib[2], (long)&size, (long)name, (long)length(name)) < 0) {
		say("(no such name)\n");
		return;
	}
	unsigned depth = (unsigned)(size / sizeof(int));
	size = sizeof(format);
	if (syscall6(SYS___sysctl, (long)mib, depth + 2, (long)&format, (long)&size, 0, 0) < 0) {
		say("(no format)\n");
		return;
	}
	const char *fmt = format.bytes + sizeof(uint32_t);
	int type = (int)(format.kind & CTLTYPE_MASK), is_unsigned = fmt[1] == 'U';
	if (type == CTLTYPE_NODE) {
		say("(a node: its children have the values)\n");
		return;
	}
	if (setting != NULL) {
		int64_t number = setting[0] == '-' ? -(int64_t)parse_number(setting + 1) : (int64_t)parse_number(setting);
		int32_t number32 = (int32_t)number;
		const void *new_value = type == CTLTYPE_STRING ? (const void *)setting : type == CTLTYPE_INT ? (const void *)&number32 : &number;
		size_t new_size = type == CTLTYPE_STRING ? length(setting) : type == CTLTYPE_INT ? 4 : 8;
		long error = type == CTLTYPE_STRING || type == CTLTYPE_INT || type == CTLTYPE_QUAD ?
		    syscall6(SYS___sysctl, (long)&mib[2], depth, 0, 0, (long)new_value, (long)new_size) : -22;
		if (error < 0) {
			say("(not taken: ");
			say_error(error);
			say(")\n");
			return;
		}
	}
	size = sizeof(value) - 1;
	if (syscall6(SYS___sysctl, (long)&mib[2], depth, (long)&value, (long)&size, 0, 0) < 0) {
		say("(the kernel will not say)\n");
		return;
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
	} else if (same(fmt, "S,timeval") && size == 16) {
		/* (a struct timeval: seconds since 1970 and microseconds, as kern.boottime has it) */
		say_number((uint64_t)value.i64[0]);
		say(" s ");
		say_number((uint32_t)value.i32[2]);
		say(" us since 1970");
	} else {
		say("(");
		say_number(size);
		say(" bytes of ");
		say(fmt);
		say(")");
	}
	say("\n");
}

/*
 * The virtual memory's counts, as vm_stat asks them: host_statistics64 (mach_host.defs, routine 219) in a
 * MIG message to the host's port, flavour HOST_VM_INFO64, a vm_statistics64 back.
 */
#define TRAP_reply_port         -26
#define TRAP_host_self          -29
#define TRAP_mach_msg2          -47
#define MIG_host_statistics64   219
#define HOST_VM_INFO64          4
#define VM_INFO64_COUNT         38              /* (sizeof(vm_statistics64_data_t) / sizeof(integer_t)) */

struct vm_statistics64 {
	uint32_t free, active, inactive, wired;
	uint64_t zero_fill, reactivations, pageins, pageouts, faults, cow_faults, lookups, hits, purges;
	uint32_t purgeable, speculative;
	uint64_t decompressions, compressions, swapins, swapouts;
	uint32_t compressor, throttled, external, internal;
	uint64_t uncompressed_in_compressor;
};

union vm_message {
	struct {
		uint32_t bits, size, remote, local, voucher;
		int32_t id;
		uint64_t ndr;
	} head;
	struct { uint8_t head[32]; int32_t flavor; uint32_t count; } request;
	struct { uint8_t head[32]; int32_t code; uint32_t count; uint32_t info[VM_INFO64_COUNT]; } reply;
	uint8_t room[512];
};

static void
say_pages(const char *what, uint64_t pages)
{
	say(what);
	say_number(pages);
}

void
command_vm(int argc, char **argv)
{
	union vm_message message = { 0 };
	struct vm_statistics64 vm;
	uint32_t host = (uint32_t)trap8(TRAP_host_self, 0, 0, 0, 0, 0, 0, 0, 0);
	uint32_t reply = (uint32_t)trap8(TRAP_reply_port, 0, 0, 0, 0, 0, 0, 0, 0);
	uint32_t bits = 19 | 21 << 8;           /* MACH_MSG_TYPE_COPY_SEND to the host, MAKE_SEND_ONCE for the answer */
	uint32_t size = sizeof(message.request);
	uint64_t page_size;
	long kr;

	(void)argc;
	(void)argv;
	message.head.bits = bits;
	message.head.size = size;
	message.head.remote = host;
	message.head.local = reply;
	message.head.id = MIG_host_statistics64;
	message.head.ndr = 0x0000000001000000ull;  /* NDR_record: little-endian, ASCII, IEEE floats */
	message.request.flavor = HOST_VM_INFO64;
	message.request.count = VM_INFO64_COUNT;
	/* MACH64_SEND_MSG | MACH64_RCV_MSG | MACH64_SEND_KOBJECT_CALL: the host is a kernel object */
	kr = trap8(TRAP_mach_msg2, (long)&message, (long)(0x1ull | 0x2ull | 0x200000000ull), (long)(bits | (uint64_t)size << 32),
	    (long)(host | (uint64_t)reply << 32), (long)((uint64_t)MIG_host_statistics64 << 32), (long)((uint64_t)reply << 32),
	    sizeof(message), 0);
	if (kr != 0 || message.reply.code != 0 || message.reply.count != VM_INFO64_COUNT) {
		say("vm: host_statistics64: ");
		say_number((uint64_t)(kr != 0 ? kr : message.reply.code));
		say(", ");
		say_number(message.reply.count);
		say(" words\n");
		return;
	}
	copy_bytes(&vm, message.reply.info, sizeof(vm));
	if (!sysctl_number("hw.pagesize", &page_size)) {
		page_size = 0;
	}
	say_pages("vm: pages of ", page_size >> 10);
	say_pages(" KB: free ", vm.free);
	say_pages(", active ", vm.active);
	say_pages(", inactive ", vm.inactive);
	say_pages(", speculative ", vm.speculative);
	say_pages(", wired ", vm.wired);
	say_pages(", in the compressor ", vm.compressor);
	say_pages("\nvm: faults ", vm.faults);
	say_pages(" (copy on write ", vm.cow_faults);
	say_pages(", zero filled ", vm.zero_fill);
	say_pages("), page-ins ", vm.pageins);
	say_pages(", page-outs ", vm.pageouts);
	say("\n");
}

void
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

#define PROC_INFO_CALL_KERNMSGBUF 4

/* [LINES]: the last LINES lines (20 if not said) of the kernel's message buffer */
void
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

/* the date and time, UTC, from the seconds since 1970 (the civil calendar's days, without a library) */
void
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

#define SYS_getentropy          500

/* [KB]: the kernel's random bytes: two of getentropy's differ, and it takes no more than 256 at once (the system
 * call says EINVAL; libc's getentropy() says EIO), and KB of /dev/random has every byte value in it,
 * each about as often as the others (none under half or over twice its share) */
void
command_random(int argc, char **argv)
{
	static uint8_t bytes[65536];
	uint8_t a[32], b[32];
	static uint32_t counts[256];
	unsigned kb = argc > 1 ? parse_number(argv[1]) : 64;
	uint32_t fewest = UINT32_MAX, most = 0, share;
	long fd, got = 0, n;
	size_t at;
	char line[200];

	/* (fewer than 16 KB, too few of each value for so narrow a judgement) */
	if (argc > 2 || kb < 16 || kb > sizeof(bytes) / 1024) {
		say("random [KB] (16 to 64)\n");
		return;
	}
	at = append(line, 0, sizeof(line), "random: getentropy ");
	if ((n = syscall6(SYS_getentropy, (long)a, sizeof(a), 0, 0, 0, 0)) != 0 ||
	    (n = syscall6(SYS_getentropy, (long)b, sizeof(b), 0, 0, 0, 0)) != 0) {
		say("random: getentropy: ");
		say_error(n);
		say("\n");
		return;
	}
	at = append(line, at, sizeof(line), bytes_same(a, b, sizeof(a)) ? "gave the same twice" : "gave two that differ");
	at = append(line, at, sizeof(line), syscall6(SYS_getentropy, (long)bytes, 257, 0, 0, 0, 0) == -22 ? " (and EINVAL past 256)" :
	    " (and NOT EINVAL past 256)");
	if ((fd = syscall6(SYS_open, (long)"/dev/random", 0, 0, 0, 0, 0)) < 0) {
		say_failed("random", "/dev/random", fd);
		return;
	}
	while ((size_t)got < kb * 1024 && (n = syscall6(SYS_read, fd, (long)(bytes + got), (long)(kb * 1024 - (size_t)got), 0, 0, 0)) > 0) {
		got += n;
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	for (int v = 0; v < 256; v++) {
		counts[v] = 0;          /* (a loop, not an initializer: there is no bzero here to call) */
	}
	for (long i = 0; i < got; i++) {
		counts[bytes[i]]++;
	}
	for (int v = 0; v < 256; v++) {
		fewest = counts[v] < fewest ? counts[v] : fewest;
		most = counts[v] > most ? counts[v] : most;
	}
	share = (uint32_t)(got / 256);
	at = append(line, at, sizeof(line), "; /dev/random: ");
	at = append_number(line, at, sizeof(line), (uint64_t)got);
	at = append(line, at, sizeof(line), " bytes, each value ");
	at = append_number(line, at, sizeof(line), fewest);
	at = append(line, at, sizeof(line), " to ");
	at = append_number(line, at, sizeof(line), most);
	at = append(line, at, sizeof(line), " times of ");
	at = append_number(line, at, sizeof(line), share);
	at = append(line, at, sizeof(line), fewest * 2 >= share && most <= share * 2 ? ": even\n" : ": NOT even\n");
	say(line);
}
