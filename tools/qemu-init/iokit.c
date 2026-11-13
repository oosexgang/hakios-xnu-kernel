/* tools/qemu-init: IOKit's registry, asked as Darwin's IOKit.framework asks it, in MIG messages: ioreg */
#include "init.h"

/* osfmk/kern/syscall_sw.c: Mach's traps */
#define TRAP_port_deallocate    -18
#define TRAP_reply_port         -26
#define TRAP_task_self          -28
#define TRAP_host_self          -29
#define TRAP_mach_msg2          -47

#define MACH_MSG_TYPE_COPY_SEND         19
#define MACH_MSG_TYPE_MAKE_SEND_ONCE    21
#define MACH_MSGH_BITS_COMPLEX          0x80000000u
#define MACH64_SEND_MSG                 0x1ull
#define MACH64_RCV_MSG                  0x2ull
#define MACH64_SEND_KOBJECT_CALL        0x200000000ull  /* (mach_msg2: the destination is the kernel's object) */

/* the MIG routines' numbers: mach_host.defs (host 200) and device.defs (iokit 2800), as the kernel's MIG made them */
#define MIG_host_get_io_main                    205
#define MIG_io_object_get_class                 2800
#define MIG_io_iterator_next                    2802
#define MIG_io_registry_entry_get_child_iterator 2813
#define MIG_io_registry_get_root_entry          2827
#define MIG_io_registry_entry_get_name_in_plane 2843

#define IOREG_DEPTH_MAX         12

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

struct header {                         /* mach_msg_header_t */
	uint32_t bits, size, remote_port, local_port, voucher_port;
	int32_t id;
};

struct port_reply {                     /* a routine's answer that is a port: a complex message, one descriptor */
	struct header head;
	uint32_t descriptors;
	uint32_t name, pad1, disposition_and_type;
};

struct error_reply {                    /* mig_reply_error_t: what comes instead when the routine failed */
	struct header head;
	uint64_t ndr;
	int32_t code;
};

struct string_request {                 /* a request that is a string (a plane's name) */
	struct header head;
	uint64_t ndr;
	uint32_t offset, count;
	char text[128];
};

struct string_reply {                   /* an answer that is a string (a name, a class) */
	struct header head;
	uint64_t ndr;
	int32_t code;
	uint32_t offset, count;
	char text[128];
};

union message {
	struct header head;
	struct port_reply port;
	struct error_reply error;
	struct string_request request;
	struct string_reply string;
	uint8_t room[1024];             /* (and the trailer the kernel adds on receiving) */
};

static uint32_t reply_port;

/* a routine of the kernel's object TARGET: SIZE bytes of MESSAGE sent, the answer received into it */
static long
call(uint32_t target, int32_t id, union message *message, uint32_t size)
{
	uint32_t bits = MACH_MSG_TYPE_COPY_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE << 8;

	if (reply_port == 0) {
		reply_port = (uint32_t)trap8(TRAP_reply_port, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	message->head = (struct header){ bits, size, target, reply_port, 0, id };
	return trap8(TRAP_mach_msg2, (long)message, (long)(MACH64_SEND_MSG | MACH64_RCV_MSG | MACH64_SEND_KOBJECT_CALL),
	    (long)(bits | (uint64_t)size << 32), (long)(target | (uint64_t)reply_port << 32), (long)((uint64_t)(uint32_t)id << 32),
	    (long)((uint64_t)reply_port << 32), sizeof(*message), 0);
}

/* a routine whose answer is a port: the port, or 0 (and *code says why, if it can) */
static uint32_t
call_for_port(uint32_t target, int32_t id, union message *message, uint32_t size, long *code)
{
	long kr = call(target, id, message, size);

	if (kr != 0 || !(message->head.bits & MACH_MSGH_BITS_COMPLEX) || message->port.descriptors != 1) {
		*code = kr != 0 ? kr : message->error.code;
		return 0;
	}
	*code = 0;
	return message->port.name;
}

static void
release(uint32_t name)
{
	if (name != 0) {
		trap8(TRAP_port_deallocate, trap8(TRAP_task_self, 0, 0, 0, 0, 0, 0, 0, 0), name, 0, 0, 0, 0, 0, 0);
	}
}

/* a request whose one argument is the string TEXT; its size */
static uint32_t
string_request(union message *message, const char *text)
{
	uint32_t n = (uint32_t)length(text) + 1;

	message->request.ndr = 0x0000000001000000ull;  /* NDR_record: little-endian, ASCII, IEEE floats */
	message->request.offset = 0;
	message->request.count = n;
	for (uint32_t i = 0; i < n; i++) {
		message->request.text[i] = text[i];
	}
	return (uint32_t)(sizeof(struct header) + 8 + 8 + ((n + 3) & ~3u));
}

/* a routine whose answer is a string, into OUT (128 bytes); whether it answered */
static int
call_for_string(uint32_t target, int32_t id, union message *message, uint32_t size, char *out)
{
	if (call(target, id, message, size) != 0 || message->string.code != 0 || message->string.count > 128) {
		return 0;
	}
	for (uint32_t i = 0; i < message->string.count; i++) {
		out[i] = message->string.text[i];
	}
	out[message->string.count > 0 ? message->string.count - 1 : 0] = '\0';
	return 1;
}

static unsigned ioreg_entries;

/* ENTRY and what is under it in the IOService plane, to DEPTH more levels */
static void
walk(uint32_t entry, int level, int depth)
{
	union message message;
	char name[128], class[128], line[320];
	long code;

	name[0] = class[0] = '?';
	name[1] = class[1] = '\0';

	call_for_string(entry, MIG_io_registry_entry_get_name_in_plane, &message, string_request(&message, "IOService"), name);
	call_for_string(entry, MIG_io_object_get_class, &message, sizeof(struct header), class);
	size_t at = 0;
	for (int i = 0; i < level; i++) {
		at = append(line, at, sizeof(line), "| ");
	}
	at = append(line, append(line, at, sizeof(line), "+-o "), sizeof(line), name);
	at = append(line, append(line, at, sizeof(line), "  <class "), sizeof(line), class);
	append(line, at, sizeof(line), ">\n");
	say(line);
	ioreg_entries++;
	if (depth == 0) {
		return;
	}
	uint32_t children = call_for_port(entry, MIG_io_registry_entry_get_child_iterator, &message,
	    string_request(&message, "IOService"), &code);
	for (uint32_t child; children != 0 &&
	    (child = call_for_port(children, MIG_io_iterator_next, &message, sizeof(struct header), &code)) != 0;) {
		walk(child, level + 1, depth - 1);
		release(child);
	}
	release(children);
}

/* [DEPTH]: the IOService plane of IOKit's registry from its root, DEPTH levels down (3 if not said) */
void
command_ioreg(int argc, char **argv)
{
	union message message;
	unsigned depth = argc > 1 ? parse_number(argv[1]) : 3;
	long code;

	if (depth > IOREG_DEPTH_MAX) {
		say("ioreg [DEPTH]: 0 to 12 levels\n");
		return;
	}
	uint32_t host = (uint32_t)trap8(TRAP_host_self, 0, 0, 0, 0, 0, 0, 0, 0);
	uint32_t main_port = call_for_port(host, MIG_host_get_io_main, &message, sizeof(struct header), &code);
	uint32_t root = main_port != 0 ? call_for_port(main_port, MIG_io_registry_get_root_entry, &message, sizeof(struct header), &code) : 0;
	if (root == 0) {
		say("ioreg: IOKit does not answer (error ");
		say_number((uint64_t)code);
		say(")\n");
		release(main_port);
		return;
	}
	ioreg_entries = 0;
	walk(root, 0, (int)depth);
	release(root);
	release(main_port);
	say("ioreg: ");
	say_number(ioreg_entries);
	say(" entries\n");
}
