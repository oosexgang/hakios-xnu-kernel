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
#define MIG_io_registry_entry_from_path         2809
#define MIG_io_registry_entry_get_property_bytes 2812
#define MIG_io_registry_entry_get_child_iterator 2813
#define MIG_io_registry_get_root_entry          2827
#define MIG_io_registry_entry_get_name_in_plane 2843
#define MIG_io_service_open_extended            2862
#define MIG_io_connect_method                   2865
#define MIG_io_service_get_matching_service_bin 2880    /* (the plain one, 2873, is no longer served) */

#define IOREG_DEPTH_MAX         12

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

struct bytes_reply {                    /* a property's bytes */
	struct header head;
	uint64_t ndr;
	int32_t code;
	uint32_t count;
	uint8_t bytes[4096];
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
	struct { struct header head; uint64_t ndr; uint32_t offset, count; char text[512]; } path;
	struct string_reply string;
	struct bytes_reply bytes;
	uint8_t room[4096 + 512];       /* (and the trailer the kernel adds on receiving) */
};

static uint32_t reply_port;

/*
 * A routine of the kernel's object TARGET: SIZE bytes of MESSAGE sent, the answer received into it.
 * DESCRIPTORS: how many port and memory descriptors follow the header (then a complex message).
 */
static long
call_with(uint32_t target, int32_t id, union message *message, uint32_t size, uint32_t descriptors)
{
	uint32_t bits = MACH_MSG_TYPE_COPY_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE << 8 | (descriptors ? MACH_MSGH_BITS_COMPLEX : 0);

	if (reply_port == 0) {
		reply_port = (uint32_t)trap8(TRAP_reply_port, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	message->head = (struct header){ bits, size, target, reply_port, 0, id };
	return trap8(TRAP_mach_msg2, (long)message, (long)(MACH64_SEND_MSG | MACH64_RCV_MSG | MACH64_SEND_KOBJECT_CALL),
	    (long)(bits | (uint64_t)size << 32), (long)(target | (uint64_t)reply_port << 32), (long)((uint64_t)(uint32_t)id << 32),
	    (long)(descriptors | (uint64_t)reply_port << 32), sizeof(*message), 0);
}

static long
call(uint32_t target, int32_t id, union message *message, uint32_t size)
{
	return call_with(target, id, message, size, 0);
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

/* a request whose one argument is the string TEXT (at most LIMIT bytes, its end included); its size */
static uint32_t
string_request_of(union message *message, const char *text, uint32_t limit)
{
	uint32_t n = (uint32_t)length(text) + 1;

	if (n > limit) {
		n = limit;
	}
	message->request.ndr = 0x0000000001000000ull;  /* NDR_record: little-endian, ASCII, IEEE floats */
	message->request.offset = 0;
	message->request.count = n;
	for (uint32_t i = 0; i < n; i++) {
		message->path.text[i] = i + 1 < n ? text[i] : '\0';
	}
	return (uint32_t)(sizeof(struct header) + 8 + 8 + ((n + 3) & ~3u));
}

static uint32_t
string_request(union message *message, const char *text)
{
	return string_request_of(message, text, 128);
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

/*
 * PATH KEY: a property of the registry entry at PATH (IOService:/, IODeviceTree:/chosen ...), as text
 * if it is text (a list of strings as several), else in hexadecimal.
 */
void
command_ioprop(int argc, char **argv)
{
	union message message;
	char line[600];
	long code;

	if (argc != 3) {
		say("ioprop PATH KEY: ioprop IOService:/ IOPlatformSerialNumber\n");
		return;
	}
	uint32_t host = (uint32_t)trap8(TRAP_host_self, 0, 0, 0, 0, 0, 0, 0, 0);
	uint32_t main_port = call_for_port(host, MIG_host_get_io_main, &message, sizeof(struct header), &code);
	uint32_t entry = main_port != 0 ? call_for_port(main_port, MIG_io_registry_entry_from_path, &message,
	    string_request_of(&message, argv[1], 512), &code) : 0;
	if (entry == 0) {
		say("ioprop: no entry at ");
		say(argv[1]);
		say("\n");
		release(main_port);
		return;
	}
	uint32_t size = string_request(&message, argv[2]);
	*(uint32_t *)((uint8_t *)&message + size) = sizeof(message.bytes.bytes);       /* (dataCnt: how much it may give) */
	long kr = call(entry, MIG_io_registry_entry_get_property_bytes, &message, size + 4);
	release(entry);
	release(main_port);
	if (kr != 0 || message.bytes.code != 0 || message.bytes.count > sizeof(message.bytes.bytes)) {
		say("ioprop: ");
		say(argv[1]);
		say(" has no ");
		say(argv[2]);
		say("\n");
		return;
	}
	/* text: printable, each string ended by a 0 (a device tree's list of strings is several) */
	uint32_t n = message.bytes.count, text = n > 0 && message.bytes.bytes[0] != '\0';
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = message.bytes.bytes[i];
		text &= (c >= ' ' && c < 0x7f) || (c == '\0' && (i == n - 1 || message.bytes.bytes[i + 1] != '\0'));
	}
	size_t at = append(line, 0, sizeof(line), argv[2]);
	at = append(line, at, sizeof(line), text ? " = \"" : " = <");
	for (uint32_t i = 0; i < n && at + 4 < sizeof(line); i++) {
		static const char hex[] = "0123456789abcdef";
		uint8_t c = message.bytes.bytes[i];

		if (text) {
			if (c != '\0') {
				line[at++] = (char)c;
			} else if (i + 1 < n) {
				at = append(line, at, sizeof(line), "\" \"");     /* (the next string of a list) */
			}
		} else {
			line[at++] = hex[c >> 4];
			line[at++] = hex[c & 15];
		}
	}
	line[at] = '\0';
	append(line, at, sizeof(line), text ? "\"\n" : ">\n");
	say(line);
}

/* ---- draw: a picture on the screen, through IOSurface and the framebuffer, as the window server shows one ---- */

#define MACH_MSG_PORT_DESCRIPTOR        0
#define MACH_MSG_OOL_DESCRIPTOR         1
#define MACH_MSG_VIRTUAL_COPY           1

/* MiniIOMobileFramebuffer's and MiniIOSurface's selectors (drivers/iokit) */
#define FB_SWAP_BEGIN           4
#define FB_SWAP_END             5
#define FB_DISPLAY_SIZE         8
#define SURFACE_CREATE          0
#define SWAP_SIZE               0x46c   /* the swap request: its ID at 0x50, then layer 0's surface, source, flag and place */

struct open_request {                   /* io_service_open_extended: the owning task and (no) properties */
	struct header head;
	uint32_t descriptors;
	uint32_t task, pad1, task_disposition_and_type;
	uint64_t properties;
	uint32_t properties_flags, properties_size;
	uint64_t ndr;
	uint32_t connect_type;
	uint64_t ndr2;
	uint32_t properties_count;
} __attribute__((packed, aligned(4)));

struct open_reply {
	struct header head;
	uint32_t descriptors;
	uint32_t connection, pad1, disposition_and_type;
	uint64_t ndr;
	int32_t result;
} __attribute__((packed, aligned(4)));

/* a service by its class (IOProviderClass), or 0 */
static uint32_t
service_of_class(uint32_t main_port, const char *class)
{
	union message message;
	char matching[160];
	size_t at = append(matching, 0, sizeof(matching), "<dict><key>IOProviderClass</key><string>");
	long code;

	append(matching, append(matching, at, sizeof(matching), class), sizeof(matching), "</string></dict>");
	/* (the dictionary as inline bytes, its end included -- XML is taken there as well as the binary form) */
	uint8_t *m = (uint8_t *)&message;
	uint32_t n = (uint32_t)length(matching) + 1;
	uint64_t ndr = 0x0000000001000000ull;
	copy_bytes(m + sizeof(struct header), &ndr, 8);
	copy_bytes(m + sizeof(struct header) + 8, &n, 4);
	copy_bytes(m + sizeof(struct header) + 12, matching, n);
	return call_for_port(main_port, MIG_io_service_get_matching_service_bin, &message,
	           (uint32_t)(sizeof(struct header) + 12 + ((n + 3) & ~3u)), &code);
}

/* the service's user client of type 0, or 0 */
static uint32_t
open_service(uint32_t service)
{
	union message message;
	struct open_request *r = (struct open_request *)&message;
	uint32_t task = (uint32_t)trap8(TRAP_task_self, 0, 0, 0, 0, 0, 0, 0, 0);

	r->descriptors = 2;
	r->task = task;
	r->pad1 = 0;
	r->task_disposition_and_type = (uint32_t)MACH_MSG_TYPE_COPY_SEND << 16 | (uint32_t)MACH_MSG_PORT_DESCRIPTOR << 24;
	r->properties = 0;
	r->properties_flags = (uint32_t)MACH_MSG_VIRTUAL_COPY << 8 | (uint32_t)MACH_MSG_OOL_DESCRIPTOR << 24;
	r->properties_size = 0;
	r->ndr = 0x0000000001000000ull;
	r->connect_type = 0;
	r->ndr2 = 0x0000000001000000ull;
	r->properties_count = 0;
	if (call_with(service, MIG_io_service_open_extended, &message, sizeof(*r), 2) != 0 ||
	    !(message.head.bits & MACH_MSGH_BITS_COMPLEX)) {
		return 0;
	}
	struct open_reply *reply = (struct open_reply *)&message;
	return reply->result == 0 ? reply->connection : 0;
}

/*
 * A method of a user client (io_connect_method): SCALARS in, INBAND bytes in, how many scalars and bytes it
 * may give back; what it gave is put where `out_scalars' and `out' point. The kernel's return, or -1.
 */
static long
connect_method(uint32_t connection, uint32_t selector, const uint64_t *scalars, uint32_t n_scalars, const void *inband,
    uint32_t n_inband, uint64_t *out_scalars, uint32_t n_out_scalars, void *out, uint32_t n_out)
{
	union message message;
	uint8_t *m = (uint8_t *)&message;
	uint32_t at = sizeof(struct header);

	/* (the variable arrays are packed: each part right after the one before, a part of bytes rounded to 4) */
	*(uint64_t *)(m + at) = 0x0000000001000000ull;
	at += 8;
	copy_bytes(m + at, &selector, 4);
	copy_bytes(m + at + 4, &n_scalars, 4);
	at += 8;
	copy_bytes(m + at, scalars, 8 * n_scalars);
	at += 8 * n_scalars;
	copy_bytes(m + at, &n_inband, 4);
	at += 4;
	copy_bytes(m + at, inband, n_inband);
	at += (n_inband + 3) & ~3u;
	uint64_t zero = 0;
	copy_bytes(m + at, &zero, 8);               /* ool_input, ool_input_size */
	copy_bytes(m + at + 8, &zero, 8);
	at += 16;
	copy_bytes(m + at, &n_out, 4);
	copy_bytes(m + at + 4, &n_out_scalars, 4);
	at += 8;
	copy_bytes(m + at, &zero, 8);               /* ool_output, ool_output_size */
	copy_bytes(m + at + 8, &zero, 8);
	at += 16;
	if (call(connection, MIG_io_connect_method, &message, at) != 0) {
		return -1;
	}
	/* the reply: NDR, the return, the bytes' count and bytes, the scalars' count and scalars */
	int32_t result;
	uint32_t count;
	at = sizeof(struct header) + 8;
	copy_bytes(&result, m + at, 4);
	if (result != 0) {
		return result;
	}
	copy_bytes(&count, m + at + 4, 4);
	at += 8;
	copy_bytes(out, m + at, count < n_out ? count : n_out);
	at += (count + 3) & ~3u;
	copy_bytes(&count, m + at, 4);
	copy_bytes(out_scalars, m + at + 4, 8 * (count < n_out_scalars ? count : n_out_scalars));
	return 0;
}

/* a picture: rings of colour about the middle, and the screen's frame */
static void
paint(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * row_bytes);

		for (uint32_t x = 0; x < width; x++) {
			int32_t dx = (int32_t)x - (int32_t)width / 2, dy = (int32_t)y - (int32_t)height / 2;
			uint32_t d = (uint32_t)(dx * dx + dy * dy) >> 9, r = (d * 3) & 255, g = (x * 255 / width), b = (y * 255 / height);

			if (x < 8 || y < 8 || x >= width - 8 || y >= height - 8) {
				r = g = b = 255;
			}
			row[x] = 0xff000000u | r << 16 | g << 8 | b;     /* BGRA in memory: blue first */
		}
	}
}

void
command_draw(int argc, char **argv)
{
	union message message;
	uint64_t size[2] = { 0, 0 }, swap_id = 0;
	uint8_t result[0x200];
	char props[512];
	long code;
	(void)argc;
	(void)argv;

	uint32_t host = (uint32_t)trap8(TRAP_host_self, 0, 0, 0, 0, 0, 0, 0, 0);
	uint32_t main_port = call_for_port(host, MIG_host_get_io_main, &message, sizeof(struct header), &code);
	uint32_t fb = main_port ? open_service(service_of_class(main_port, "IOMobileFramebuffer")) : 0;
	uint32_t surfaces = main_port ? open_service(service_of_class(main_port, "IOCoreSurfaceRoot")) : 0;
	if (fb == 0 || surfaces == 0) {
		say(fb == 0 ? "draw: no framebuffer to open\n" : "draw: no IOCoreSurfaceRoot to open\n");
		return;
	}
	if (connect_method(fb, FB_DISPLAY_SIZE, NULL, 0, NULL, 0, size, 2, NULL, 0) != 0 || size[0] == 0 || size[1] == 0) {
		say("draw: the framebuffer will not say its size\n");
		return;
	}
	uint32_t width = (uint32_t)size[0], height = (uint32_t)size[1];
	size_t at = append(props, 0, sizeof(props), "<dict><key>IOSurfaceWidth</key><integer>");
	at = append_number(props, at, sizeof(props), width);
	at = append(props, at, sizeof(props), "</integer><key>IOSurfaceHeight</key><integer>");
	at = append_number(props, at, sizeof(props), height);
	at = append(props, at, sizeof(props), "</integer><key>IOSurfaceBytesPerElement</key><integer>4</integer>"
	    "<key>IOSurfaceBytesPerRow</key><integer>");
	at = append_number(props, at, sizeof(props), (uint64_t)width * 4);
	at = append(props, at, sizeof(props), "</integer><key>IOSurfacePixelFormat</key><integer>1111970369</integer>"
	    "<key>IOSurfaceAllocSize</key><integer>");
	at = append_number(props, at, sizeof(props), (uint64_t)width * height * 4);
	append(props, at, sizeof(props), "</integer></dict>");
	if (connect_method(surfaces, SURFACE_CREATE, NULL, 0, props, (uint32_t)length(props) + 1, NULL, 0, result, sizeof(result)) != 0) {     /* (the description and its plane: under 0x200, Darwin 16.5's form) */
		say("draw: no surface\n");
		return;
	}
	uint64_t base;
	uint32_t sid, row_bytes;
	copy_bytes(&base, result, 8);
	copy_bytes(&sid, result + 0x18, 4);
	copy_bytes(&row_bytes, result + 0x28, 4);
	paint((uint32_t *)base, width, height, row_bytes);
	/* a swap with the surface as its one layer, the whole of it on the whole screen */
	static uint8_t swap[SWAP_SIZE];
	int32_t rect[4] = { 0, 0, (int32_t)width, (int32_t)height };
	uint32_t one = 1;
	if (connect_method(fb, FB_SWAP_BEGIN, NULL, 0, NULL, 0, &swap_id, 1, NULL, 0) != 0) {
		say("draw: no swap begun\n");
		return;
	}
	uint32_t id = (uint32_t)swap_id;
	copy_bytes(swap + 0x50, &id, 4);
	copy_bytes(swap + 0x54, &sid, 4);
	copy_bytes(swap + 0x64, rect, 16);
	copy_bytes(swap + 0xa4, &one, 4);
	copy_bytes(swap + 0xc4, rect, 16);
	copy_bytes(swap + 0x104, &one, 4);
	if (connect_method(fb, FB_SWAP_END, NULL, 0, swap, SWAP_SIZE, NULL, 0, NULL, 0) != 0) {
		say("draw: the swap was not taken\n");
		return;
	}
	char line[160];
	at = append_number(line, append(line, 0, sizeof(line), "draw: a picture of "), sizeof(line), width);
	at = append_number(line, append(line, at, sizeof(line), " x "), sizeof(line), height);
	at = append_number(line, append(line, at, sizeof(line), " on the screen: surface "), sizeof(line), sid);
	at = append_number(line, append(line, at, sizeof(line), ", swap "), sizeof(line), id);
	append(line, at, sizeof(line), "\n");
	say(line);
}
