/*
 * Parts of the loader (loader/src) that are plain C with nothing of a machine in them, compiled as
 * they are into a host program: its own memory and string functions, the writer of the device tree
 * the kernel is given, the NVRAM image, the payload's directory, the firmware's clock read as
 * seconds. What they write is read back here by code written from the formats' descriptions, not by
 * the loader's own; and what the packer writes (loader/mkpayload.py) is read by the loader's reader.
 *
 *   loader_test SCENARIO
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efi.h"
#include "identity.h"

/* the loader has its own memset, memcpy and memcmp (it links no library): under other names here, beside the host's */
#define memset loader_memset
#define memcpy loader_memcpy
#define memcmp loader_memcmp
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

/* the payload's place in memory is a constant of the phone: here, a buffer */
static _Alignas(16) uint8_t payload_area[4096];
#define PAYLOAD_BASE ((uintptr_t)payload_area)

static char said[4096];
static void
print(const char *format, ...)
{
	va_list list;

	/* (the loader's own format has %x and %d of 64 bits and %s: enough for the host's to show the text) */
	va_start(list, format);
	va_end(list);
	snprintf(said + strlen(said), sizeof(said) - strlen(said), "%s", format);
}

#include "src/conventions.h"
#include "src/base.c"
#include "src/devicetree.c"
#include "src/nvram.c"
#include "src/payload.c"
#include "src/unit.c"

#undef memset
#undef memcpy
#undef memcmp

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- memory and strings -------------------------------------------------------------------------- */

static void
memory_and_strings(void)
{
	static uint8_t from[300], to[320], expect[320];

	/* every alignment of both ends and every length up to 260: the word-at-a-time path and its edges */
	for (unsigned i = 0; i < sizeof(from); i++) {
		from[i] = (uint8_t)(i * 7 + 1);
	}
	for (unsigned a = 0; a < 9; a++) {
		for (unsigned b = 0; b < 9; b++) {
			for (unsigned n = 0; n <= 260; n += (n < 70 ? 1 : 19)) {
				memset(to, 0xee, sizeof(to));
				memset(expect, 0xee, sizeof(expect));
				memcpy(expect + 8 + b, from + a, n);
				CHECK(loader_memcpy(to + 8 + b, from + a, n) == to + 8 + b, "memcpy returns where it wrote");
				if (memcmp(to, expect, sizeof(to)) != 0) {
					CHECK(0, "memcpy of %u bytes from +%u to +%u", n, a, b);
					return;
				}
			}
		}
	}
	memset(to, 0x11, sizeof(to));
	loader_memset(to + 3, 0xabc, 17);       /* (the value is taken as a byte) */
	CHECK(to[2] == 0x11 && to[3] == 0xbc && to[19] == 0xbc && to[20] == 0x11, "memset writes 17 bytes and no more");
	loader_memset(to, 0, 0);
	CHECK(to[0] == 0x11, "... and none when asked for none");
	CHECK(loader_memcmp("abc", "abc", 3) == 0 && loader_memcmp("abc", "abd", 3) < 0 && loader_memcmp("abd", "abc", 3) > 0, "memcmp orders");
	CHECK(loader_memcmp("abc", "abd", 2) == 0 && loader_memcmp("\x80", "\x7f", 1) > 0, "... as far as told, bytes without a sign");
	CHECK(str_len("") == 0 && str_len("kernel") == 6, "str_len");
	CHECK(str_eq("usb", "usb") && !str_eq("usb", "usbx") && !str_eq("usbx", "usb") && str_eq("", "") && !str_eq("a", ""), "str_eq");
}

/* ---- the device tree, read back as its users read it --------------------------------------------- */

struct seen { char path[128]; char name[33]; uint32_t length; const uint8_t *value; };
static struct seen seen[256];
static unsigned seen_count, bad_padding, bad_name;

/* node := u32 properties, u32 children, the properties, the children; property := name[32], u32 length, value padded to 4 */
static const uint8_t *
walk(const uint8_t *at, const char *parent)
{
	uint32_t properties, children;
	char path[128] = "";

	memcpy(&properties, at, 4);
	memcpy(&children, at + 4, 4);
	at += 8;
	for (uint32_t p = 0; p < properties; p++) {
		struct seen *s = &seen[seen_count++ % 256];
		uint32_t length, padded;

		memcpy(&length, at + 32, 4);
		padded = (length + 3) & ~3u;
		memcpy(s->name, at, 32);
		s->name[32] = 0;
		for (size_t i = strlen(s->name); i < 32; i++) {
			bad_name += at[i] != 0;                 /* what follows the name in its field is zeros */
		}
		for (uint32_t i = length; i < padded; i++) {
			bad_padding += at[36 + i] != 0;         /* ... and so is the value's padding */
		}
		s->length = length;
		s->value = at + 36;
		if (p == 0) {
			snprintf(path, sizeof(path), "%s/%s", parent, (const char *)at + 36);
		}
		snprintf(s->path, sizeof(s->path), "%s", path);
		at += 36 + padded;
	}
	for (uint32_t c = 0; c < children; c++) {
		at = walk(at, path);
	}
	return at;
}

static const struct seen *
found(const char *path, const char *name)
{
	for (unsigned i = 0; i < seen_count && i < 256; i++) {
		if (strcmp(seen[i].path, path) == 0 && strcmp(seen[i].name, name) == 0) {
			return &seen[i];
		}
	}
	return NULL;
}

static void
device_tree(void)
{
	static uint8_t tree[8192];
	struct dt_writer w = { .base = tree, .cur = tree };
	const struct seen *s;
	const uint8_t *end;
	uint64_t big = 0x1122334455667788ULL;

	memset(tree, 0xaa, sizeof(tree));       /* (memory as it is found: whatever was there) */
	dt_begin(&w, "device-tree");
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_str(&w, "model", "a phone");
	dt_begin(&w, "chosen");
	dt_prop_str(&w, "darwin-board", "phone-cmd");
	dt_prop_u64(&w, "ram-size", big);
	dt_prop(&w, "seven", "1234567", 7);     /* (a value that is not a multiple of four) */
	dt_prop(&w, "nothing", "", 0);
	dt_begin(&w, "memory-map");
	dt_prop_padded(&w, "a-thirty-one-letter-name-123456", "ab", 16);
	dt_end(&w);
	dt_end(&w);
	dt_begin(&w, "cpus");
	dt_begin(&w, "cpu0");
	dt_end(&w);
	dt_begin(&w, "cpu1");
	dt_prop_u32(&w, "reg", 1);
	dt_end(&w);
	dt_end(&w);
	dt_end(&w);
	CHECK(w.depth == 0, "every node that was begun is ended");

	end = walk(tree, "");
	CHECK(end == w.cur, "the tree read back ends where the writer stopped: %ld bytes against %ld", (long)(end - tree), (long)(w.cur - tree));
	CHECK(bad_padding == 0 && bad_name == 0, "%u bytes of padding and %u after a name are not zero", bad_padding, bad_name);
	CHECK(((uintptr_t)w.cur & 3) == 0, "everything is on four bytes");
	CHECK(seen_count == 14, "eight properties and the six nodes' names: %u", seen_count);
	CHECK((s = found("/device-tree", "model")) != NULL && s->length == 8 && strcmp((const char *)s->value, "a phone") == 0, "a string, with its zero");
	CHECK((s = found("/device-tree", "#address-cells")) != NULL && s->length == 4 && s->value[0] == 2 && s->value[3] == 0, "32 bits, least significant first");
	CHECK((s = found("/device-tree/chosen", "ram-size")) != NULL && s->length == 8 && memcmp(s->value, &big, 8) == 0, "64 bits");
	CHECK((s = found("/device-tree/chosen", "seven")) != NULL && s->length == 7 && memcmp(s->value, "1234567", 7) == 0, "seven bytes stay seven");
	CHECK((s = found("/device-tree/chosen", "nothing")) != NULL && s->length == 0, "a property without a value");
	CHECK((s = found("/device-tree/chosen", "darwin-board")) != NULL && strcmp((const char *)s->value, "phone-cmd") == 0, "the board's name");
	CHECK((s = found("/device-tree/chosen/memory-map", "a-thirty-one-letter-name-123456")) != NULL && s->length == 16 && s->value[0] == 'a' &&
	    s->value[2] == 0 && s->value[15] == 0, "a name of 31 letters; a string in a field of 16");
	CHECK(found("/device-tree/cpus/cpu0", "name") != NULL && (s = found("/device-tree/cpus/cpu1", "reg")) != NULL && s->value[0] == 1,
	    "two children of one node, each with its own");
	CHECK(found("/device-tree/cpus", "reg") == NULL && found("/device-tree", "darwin-board") == NULL, "nothing lands in another node");
}

/* the iPhone's own properties are copied as they are, but the display's gamut */
static void
identity(void)
{
	static uint8_t tree[65536];
	struct dt_writer w = { .base = tree, .cur = tree };
	const struct seen *s;
	unsigned count = sizeof(identity_product) / sizeof(identity_product[0]);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, count, identity_product_data);
	dt_end(&w);
	CHECK(walk(tree, "") == w.cur, "the tree reads back whole");
	CHECK(seen_count == count + 1, "every property of the table, and the node's name: %u of %u", seen_count, count + 1);
	CHECK((s = found("/product", "artwork-display-gamut")) != NULL && s->length == 5 && strcmp((const char *)s->value, "sRGB") == 0,
	    "the gamut is the framebuffer's, not the iPhone's P3");
	CHECK((s = found("/product", "artwork-scale-factor")) != NULL && s->length == 4 && memcmp(s->value, identity_product_data + 4, 4) == 0,
	    "another property is the iPhone's own bytes");

	seen_count = 0;
	gamut_p3 = 1;
	w = (struct dt_writer){ .base = tree, .cur = tree };
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, count, identity_product_data);
	dt_identity_one(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data, "model");
	dt_identity_one(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data, "no-such-property");
	dt_end(&w);
	walk(tree, "");
	CHECK((s = found("/product", "artwork-display-gamut")) != NULL && s->length == 3 && memcmp(s->value, "P3", 3) == 0, "gamut=p3 leaves it");
	CHECK(seen_count == count + 2 && (s = found("/product", "model")) != NULL && s->length == 11, "one property by its name, and only that one");
}

/* ---- the firmware's clock -------------------------------------------------------------------------- */

static struct efi_time clock_says;
static efi_status clock_status;

static efi_status EFIAPI
get_time(struct efi_time *time, void *capabilities)
{
	(void)capabilities;
	*time = clock_says;
	return clock_status;
}

static uint64_t
epoch_of(int year, int month, int day, int hour, int minute, int second, int timezone)
{
	clock_says = (struct efi_time){ .year = (uint16_t)year, .month = (uint8_t)month, .day = (uint8_t)day, .hour = (uint8_t)hour,
		                        .minute = (uint8_t)minute, .second = (uint8_t)second, .timezone = (int16_t)timezone };
	return firmware_epoch();
}

static void
clock_as_seconds(void)
{
	static efi_runtime_services runtime;
	static efi_system_table table;

	ST = &table;
	CHECK(firmware_epoch() == 0, "a firmware without runtime services has no clock");
	table.rt = &runtime;
	CHECK(firmware_epoch() == 0, "... nor one without the call");
	runtime.get_time = get_time;
	/* (what a calendar says for these moments) */
	CHECK(epoch_of(2026, 11, 11, 12, 30, 45, 0x7ff) == 1794400245ULL, "a day in November: %llu", (unsigned long long)firmware_epoch());
	CHECK(epoch_of(2024, 2, 29, 0, 0, 0, 0x7ff) == 1709164800ULL, "the 29th of February of a leap year");
	CHECK(epoch_of(2024, 3, 1, 0, 0, 0, 0x7ff) == 1709251200ULL, "the day after it");
	CHECK(epoch_of(2025, 1, 1, 0, 0, 0, 0x7ff) == 1735689600ULL, "the first day after a leap year");
	CHECK(epoch_of(2028, 12, 31, 23, 59, 59, 0x7ff) == 1861919999ULL, "the last second of a leap year");
	CHECK(epoch_of(2099, 1, 1, 0, 0, 0, 0x7ff) == 4070908800ULL, "the last year taken");
	/* local time is UTC less the zone's minutes: 12:30 at +3 h is said with a zone of -180 */
	CHECK(epoch_of(2026, 11, 11, 15, 30, 45, -180) == 1794400245ULL, "a clock that keeps local time, east of Greenwich");
	CHECK(epoch_of(2026, 11, 11, 7, 30, 45, 300) == 1794400245ULL, "... and west of it");
	CHECK(epoch_of(2026, 11, 11, 12, 30, 45, 0) == 1794400245ULL, "... and one that says it keeps UTC");
	CHECK(epoch_of(2023, 6, 1, 0, 0, 0, 0x7ff) == 0 && epoch_of(1980, 1, 6, 0, 0, 0, 0x7ff) == 0, "a clock that was never set is no clock");
	CHECK(epoch_of(2100, 1, 1, 0, 0, 0, 0x7ff) == 0 && epoch_of(2026, 0, 1, 0, 0, 0, 0x7ff) == 0 && epoch_of(2026, 13, 1, 0, 0, 0, 0x7ff) == 0,
	    "nor one that says what cannot be");
	clock_status = EFI_ERROR_BIT | 7;
	CHECK(epoch_of(2026, 11, 11, 12, 30, 45, 0x7ff) == 0, "nor one whose call fails");
}

/* ---- the NVRAM image, checked as the kernel checks it ---------------------------------------------- */

static void
nvram(void)
{
	static uint8_t image[NVRAM_BANK_SIZE + 64];
	uint32_t a = 1, b = 0, stored, generation;

	memset(image, 0x5c, sizeof(image));
	build_nvram_image(image);
	for (unsigned i = NVRAM_BANK_SIZE; i < sizeof(image); i++) {
		if (image[i] != 0x5c) {
			CHECK(0, "byte %u after the image was written", i);
			break;
		}
	}
	/* a CHRP header: signature, checksum, length in blocks of 16 bytes, a name of 12 */
	for (unsigned at = 0; at <= 0x20; at += 0x20) {
		unsigned sum = image[at];

		for (unsigned i = 2; i < 16; i++) {
			sum += image[at + i];
		}
		sum = (sum & 0xff) + (sum >> 8);
		sum = (sum & 0xff) + (sum >> 8);
		CHECK(image[at + 1] == sum, "the header at %#x: checksum %#x, its bytes add up to %#x", at, image[at + 1], sum);
	}
	CHECK(image[0] == 0x5a && image[2] == 2 && image[3] == 0 && memcmp(image + 4, "nvram\0\0\0\0\0\0\0", 12) == 0, "Apple's header: two blocks, \"nvram\"");
	CHECK(image[0x20] == 0x70 && (image[0x22] | image[0x23] << 8) == (NVRAM_BANK_SIZE - 0x20) / 16 && memcmp(image + 0x24, "common", 7) == 0,
	    "the common partition takes the rest");
	/* Adler-32 of everything after the checksum's own field */
	for (unsigned i = 20; i < NVRAM_BANK_SIZE; i++) {
		a = (a + image[i]) % 65521;
		b = (b + a) % 65521;
	}
	memcpy(&stored, image + 16, 4);
	memcpy(&generation, image + 20, 4);
	CHECK(stored == (b << 16 | a), "Adler-32 %#x, the image has %#x", b << 16 | a, stored);
	CHECK(generation == 1, "the first generation");
	for (unsigned i = 0x30; i < NVRAM_BANK_SIZE; i++) {
		if (image[i] != 0) {
			CHECK(0, "the common partition is not empty at %#x", i);
			break;
		}
	}
}

/* ---- the payload's directory ----------------------------------------------------------------------- */

static uint8_t *payload_at;

static void
payload_begin(void)
{
	memset(payload_area, 0, sizeof(payload_area));
	payload_at = payload_area + sizeof(struct payload_header);
	((struct payload_header *)payload_area)->magic = PAYLOAD_MAGIC;
	said[0] = 0;
}

/* an entry as loader/mkpayload.py writes one: name, size, the bytes, padded to 16 */
static const uint8_t *
entry(const char *name, const void *bytes, uint32_t size)
{
	struct payload_entry *e = (struct payload_entry *)payload_at;
	const uint8_t *data = payload_at + sizeof(*e);

	memset(e->name, 0, sizeof(e->name));
	memcpy(e->name, name, strlen(name) < sizeof(e->name) ? strlen(name) : sizeof(e->name));
	e->size = size;
	memcpy(payload_at + sizeof(*e), bytes, size);
	payload_at += sizeof(*e) + ((size + 15) & ~15u);
	((struct payload_header *)payload_area)->size = (uint32_t)(payload_at - payload_area - sizeof(struct payload_header));
	return data;
}

static void
payload(void)
{
	struct payload_info info;
	const uint8_t *kernel, *ramdisk;

	CHECK(parse_number("1f0", 16) == 0x1f0 && parse_number("42", 10) == 42 && parse_number("12ab", 10) == 12 && parse_number("", 16) == 0 &&
	    parse_number("ffffffffffffffff", 16) == UINT64_MAX && parse_number("9 ", 10) == 9 && parse_number("x", 16) == 0, "numbers");

	payload_begin();
	kernel = entry("kernel", "\xcf\xfa\xed\xfe and so on", sizeof("\xcf\xfa\xed\xfe and so on"));
	entry("cmdline", "debug=0x14e cpus=8 wifi=0", 26);
	ramdisk = entry("ramdisk", "an image", 9);
	entry("checkpoint", "17", 3);
	entry("physbase", "a2000000", 9);
	entry("rdres", "4c000000", 9);
	entry("something", "new", 4);                   /* (an entry a newer packer adds is passed over) */
	CHECK(parse_payload(&info, 1) == 1, "a payload is taken: %s", said);
	CHECK(info.kernel == kernel && info.ramdisk == ramdisk && info.ramdisk_size == 9, "the kernel and the root image where they are");
	CHECK(strcmp(info.cmdline, "debug=0x14e cpus=8 wifi=0") == 0, "the boot arguments: %s", info.cmdline);
	CHECK(info.checkpoint == 17 && info.forced_base == 0xa2000000ULL && info.resident_rd == 0x4c000000ULL, "the numbers");
	CHECK(info.usb_mode == 0, "no \"usb\" entry: the kernel is run");
	CHECK(strstr(said, "payload entry") != NULL, "said when asked to");

	payload_begin();
	entry("usb", "", 0);
	CHECK(parse_payload(&info, 0) == 1 && info.usb_mode == 1 && info.kernel == NULL && info.cmdline[0] == 0, "a payload that only says: take commands");
	CHECK(said[0] == 0, "nothing said when not asked");

	payload_begin();
	((struct payload_header *)payload_area)->magic = 0x12345678;
	CHECK(parse_payload(&info, 0) == 0 && info.kernel == NULL && info.cmdline[0] == 0, "what is not a payload is none");
	CHECK(strstr(said, "no payload") != NULL, "and that is said");

	/* the directory ends at an empty name, or where the header says */
	payload_begin();
	entry("kernel", "k", 2);
	payload_at += sizeof(struct payload_entry);
	((struct payload_header *)payload_area)->size += sizeof(struct payload_entry);
	entry("usb", "", 0);
	CHECK(parse_payload(&info, 0) == 1 && info.kernel != NULL && info.usb_mode == 0, "nothing is read after an empty name");
	payload_begin();
	entry("kernel", "k", 2);
	entry("usb", "", 0);
	((struct payload_header *)payload_area)->size -= sizeof(struct payload_entry);
	CHECK(parse_payload(&info, 0) == 1 && info.kernel != NULL && info.usb_mode == 0, "nor past the size the header gives");
}

/* what the packer writes (loader/mkpayload.py), the loader reads: the two are run against each other */
static int
packed(const char *directory, const char *arguments)
{
	char command[2048], tools[1024], path[1024];
	FILE *file;
	size_t length;

	/* (the packer is beside the loader: this file is workbench/loader_test.c of the same tree) */
	snprintf(tools, sizeof(tools), "%s", __FILE__);
	*strrchr(tools, '/') = 0;
	snprintf(path, sizeof(path), "%s/payload.bin", directory);
	snprintf(command, sizeof(command), "cd '%s' && python3 '%s/../loader/mkpayload.py' -o payload.bin %s > /dev/null", directory, tools,
	    arguments);
	if (system(command) != 0 || (file = fopen(path, "rb")) == NULL) {
		return 0;
	}
	memset(payload_area, 0xee, sizeof(payload_area));       /* (whatever was in memory before the payload) */
	length = fread(payload_area, 1, sizeof(payload_area), file);
	fclose(file);
	said[0] = 0;
	return length > 8 && length < sizeof(payload_area);
}

static void
packed_payload(void)
{
	char directory[512], command[1024];
	struct payload_info info;

	snprintf(directory, sizeof(directory), "%s/loader_test.XXXXXX", getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	if (mkdtemp(directory) == NULL) {
		CHECK(0, "no directory to work in");
		return;
	}
	snprintf(command, sizeof(command), "cd '%s' && printf 'the kernel, 21 bytes.' > k && printf 'an image of 20 bytes' > r", directory);
	CHECK(system(command) == 0, "the files to pack");
	CHECK(packed(directory, "kernel=k ramdisk=r --cmdline 'debug=0x14e cpus=8' --physbase 0xA2000000 --rdres 4C000000 --checkpoint 17"), "packed");
	CHECK(parse_payload(&info, 0) == 1, "the loader takes what the packer wrote: %s", said);
	CHECK(info.kernel != NULL && memcmp(info.kernel, "the kernel, 21 bytes.", 21) == 0, "the kernel's bytes");
	CHECK(info.ramdisk != NULL && info.ramdisk_size == 20 && memcmp(info.ramdisk, "an image of 20 bytes", 20) == 0, "the image and its size");
	CHECK(strcmp(info.cmdline, "debug=0x14e cpus=8") == 0, "the boot arguments: %s", info.cmdline);
	CHECK(info.forced_base == 0xa2000000ULL && info.resident_rd == 0x4c000000ULL && info.checkpoint == 17 && info.usb_mode == 0,
	    "the numbers, however their hexadecimal was written: %llx %llx %llu", (unsigned long long)info.forced_base,
	    (unsigned long long)info.resident_rd, (unsigned long long)info.checkpoint);
	CHECK(packed(directory, "--usb"), "packed with --usb alone");
	CHECK(parse_payload(&info, 0) == 1 && info.usb_mode == 1 && info.kernel == NULL && info.cmdline[0] == 0, "a payload that only says: take commands");
	snprintf(command, sizeof(command), "rm -rf '%s'", directory);
	CHECK(system(command) == 0, "the directory removed");
}

/* this unit's identity (src/unit.c): each value of the form a real one has, the same from the same seed, another from another */
static void
unit(void)
{
	static const char alphabet[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	struct unit_identity a, b, c;
	uint64_t seed = 0;

	unit_from_seed(0x0123456789abcdefULL, &a);
	unit_from_seed(0x0123456789abcdefULL, &b);
	unit_from_seed(0x0123456789abcdeeULL, &c);
	CHECK(strlen(a.serial) == 10 && strspn(a.serial, alphabet) == 10, "a serial number of 10 of the alphabet: %s", a.serial);
	CHECK(strlen(a.mlb) == 17 && strspn(a.mlb, alphabet) == 17, "an MLB serial of 17: %s", a.mlb);
	CHECK(a.ecid != 0 && a.ecid >> 52 == 0, "a 52-bit ECID: %llx", (unsigned long long)a.ecid);
	CHECK((a.wifi_address[0] & 3) == 2 && (a.bluetooth_address[0] & 3) == 2, "unicast, locally administered addresses");
	CHECK(memcmp(a.wifi_address, a.bluetooth_address, 6) != 0, "the two addresses differ");
	CHECK(memcmp(&a, &b, sizeof(a)) == 0, "the same seed, the same identity");
	CHECK(strcmp(a.serial, "7BN0E0VZPK") == 0 && strcmp(a.mlb, "5JTH1PNG6UYUS1AA7") == 0 && a.ecid == 0xb77554620b3ddULL &&
	    memcmp(a.wifi_address, "\x2e\x2c\x55\x46\x7d\x6d", 6) == 0, "what tools/identity.py says of this seed (workbench/identity_test.py)");
	CHECK(strcmp(a.serial, c.serial) != 0 && a.ecid != c.ecid && memcmp(a.wifi_address, c.wifi_address, 6) != 0 &&
	    memcmp(a.manifest_hash, c.manifest_hash, 48) != 0, "another seed, another identity");
	/* serial numbers of many seeds: no two alike, and every character of the alphabet in use */
	{
		static char seen[2000][11];
		unsigned used[34] = { 0 }, same = 0;

		for (unsigned i = 0; i < 2000; i++) {
			unit_from_seed(i, &a);
			memcpy(seen[i], a.serial, 11);
			for (unsigned k = 0; k < 10; k++) {
				used[strchr(alphabet, a.serial[k]) - alphabet]++;
			}
			for (unsigned j = 0; j < i; j++) {
				same += strcmp(seen[i], seen[j]) == 0;
			}
		}
		CHECK(same == 0, "2000 seeds, 2000 serial numbers: %u alike", same);
		for (unsigned k = 0; k < 34; k++) {
			CHECK(used[k] > 300 && used[k] < 900, "character %c, %u times in 20000", alphabet[k], used[k]);
		}
	}
	/* the seed among the boot arguments */
	CHECK(unit_seed_from_args("rd=md0 identity=00112233AABBccdd debug=1", &seed) == 1 && seed == 0x00112233aabbccddULL,
	    "identity= read: %llx", (unsigned long long)seed);
	CHECK(unit_seed_from_args("identity=0011223344556677", &seed) == 1 && seed == 0x0011223344556677ULL, "at the start");
	CHECK(unit_seed_from_args("rd=md0", &seed) == 0, "none given");
	CHECK(unit_seed_from_args("identity=123", &seed) == 0, "too short");
	CHECK(unit_seed_from_args("identity=00112233445566778", &seed) == 0, "too long");
	CHECK(unit_seed_from_args("identity=001122334455667g", &seed) == 0, "not hex");
	CHECK(unit_seed_from_args("myidentity=0011223344556677", &seed) == 0, "another argument whose name ends so");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "memory_and_strings", memory_and_strings }, { "device_tree", device_tree }, { "identity", identity },
		{ "unit", unit }, { "clock_as_seconds", clock_as_seconds }, { "nvram", nvram }, { "payload", payload }, { "packed_payload", packed_payload },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
