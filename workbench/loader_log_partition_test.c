/*
 * The loader's log written into the phone's "logdump" partition (arch/arm64/boot/src/logstore.c), compiled as it
 * is into a host program, with a made-up firmware of three partitions behind the calls it makes. It is a raw
 * write to the phone's flash, from the partition's first block: so it has to go into the partition of exactly
 * that name and no other, stay inside it whatever the log's length, and say "read back the same" only of blocks
 * that are on the flash.
 *
 *   loader_log_partition_test SCENARIO
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "efi.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

/* the loader has its own memory functions and its own format (it links no library): under other names here */
#define memset loader_memset
#define memcpy loader_memcpy
#define memcmp loader_memcmp
#define memmove loader_memmove
#include "src/base.c"
#undef memset
#undef memcpy
#undef memcmp
#undef memmove

/* what the loader says goes into its log, as console.c puts it there, and is kept here to be looked at */
static char said[16384];

static void
put_char(char c)
{
	size_t at = strlen(said);

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (at + 1 < sizeof(said)) {
		said[at] = c;
		said[at + 1] = 0;
	}
}

static void
console_put(char c, void *unused)
{
	put_char(c);
}

static void
print(const char *format, ...)
{
	__builtin_va_list list;

	__builtin_va_start(list, format);
	format_with(console_put, NULL, format, list);
	__builtin_va_end(list);
}

#include "src/conventions.h"
#include "src/logstore.c"

/* ... and whether UEFI's transfer list is the one in the UFS controller (ufs_block.c: the kernel may have put its own in) */
static bool kernels_list_in;

static int
hc_list_is_uefis(void)
{
	return !kernels_list_in;
}

static void
hc_print_state(const char *when)
{
}

static void
hc_take_back(const char *when)
{
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- a phone's partitions: "logdump_b", "logdump" and "modem", each with its own blocks ----------------------- */

#define PARTS           3
#define LOGDUMP         1
#define BLOCKS          1024                    /* 4 MB each, as far as the made-up storage goes */

static struct part {
	efi_partition_info info;
	efi_block_media media;
	efi_block_io io;
	uint8_t bytes[BLOCKS * 4096];
	unsigned writes, past_end, flushes, reads;
	uint64_t lba, size;                     /* the last write */
} parts[PARTS];
static efi_status flush_status;

static struct part *
part_of(struct efi_block_io *self)
{
	for (unsigned i = 0; i < PARTS; i++) {
		if (&parts[i].io == self) {
			return &parts[i];
		}
	}
	return NULL;
}

/* (a driver that does not look at the partition's end: what the loader writes past it is counted, not refused) */
static efi_status EFIAPI
part_write(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	struct part *p = part_of(self);
	uint64_t bs = p->media.block_size;

	p->writes++;
	p->lba = lba;
	p->size = size;
	if (media_id != p->media.media_id || bs == 0 || size % bs) {
		return EFI_ERROR_BIT | 2;
	}
	if (lba + size / bs > p->media.last_block + 1) {
		p->past_end++;
	}
	if ((lba + size / bs) * bs <= sizeof(p->bytes)) {
		memcpy(p->bytes + lba * bs, buffer, size);
	}
	return 0;
}

static efi_status EFIAPI
part_read(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	struct part *p = part_of(self);
	uint64_t bs = p->media.block_size;

	p->reads++;
	if (media_id != p->media.media_id || bs == 0 || size % bs || (lba + size / bs) * bs > sizeof(p->bytes)) {
		return EFI_ERROR_BIT | 2;
	}
	memcpy(buffer, p->bytes + lba * bs, size);
	return 0;
}

static efi_status EFIAPI
part_flush(struct efi_block_io *self)
{
	part_of(self)->flushes++;
	return flush_status;
}

static const efi_guid part_guid = EFI_PARTITION_INFO_GUID, blk_guid = EFI_BLOCK_IO_GUID;
static efi_handle handle_list[PARTS];
static unsigned handles_given;

static efi_status EFIAPI
locate_handle_buffer(int type, const efi_guid *guid, void *key, uint64_t *count, efi_handle **buffer)
{
	if (type != ByProtocol || memcmp(guid, &part_guid, sizeof(*guid)) != 0) {
		return EFI_ERROR_BIT | 14;      /* EFI_NOT_FOUND */
	}
	for (unsigned i = 0; i < PARTS; i++) {
		handle_list[i] = &parts[i];
	}
	*count = PARTS;
	*buffer = handle_list;
	handles_given++;
	return 0;
}

static efi_status EFIAPI
handle_protocol(efi_handle h, const efi_guid *guid, void **iface)
{
	struct part *p = h;

	if (memcmp(guid, &part_guid, sizeof(*guid)) == 0) {
		*iface = &p->info;
	} else if (memcmp(guid, &blk_guid, sizeof(*guid)) == 0) {
		*iface = &p->io;
	} else {
		return EFI_ERROR_BIT | 3;       /* EFI_UNSUPPORTED */
	}
	return 0;
}

static efi_status EFIAPI
free_pool(void *buffer)
{
	handles_given--;
	return 0;
}

static efi_boot_services boot_services = { .locate_handle_buffer = locate_handle_buffer, .handle_protocol = handle_protocol,
	                                   .free_pool = free_pool };

static void
phone(void)
{
	static const char *const names[PARTS] = { "logdump_b", "logdump", "modem" };

	BS = &boot_services;
	for (unsigned i = 0; i < PARTS; i++) {
		struct part *p = &parts[i];

		memset(p, 0, sizeof(*p));
		p->info.type = 2;               /* GPT */
		for (unsigned k = 0; names[i][k] != 0; k++) {
			p->info.name[k] = (char16)names[i][k];
		}
		p->media = (efi_block_media){ .media_id = 5 + i, .present = 1, .logical = 1, .block_size = 4096, .last_block = BLOCKS - 1 };
		p->io = (efi_block_io){ .media = &p->media, .read_blocks = part_read, .write_blocks = part_write, .flush_blocks = part_flush };
	}
	flush_status = 0;
	kernels_list_in = false;
	said[0] = 0;
	log_len = 0;
	kernel_part_len = 0;                    /* (as "oem run" begins: the last run's kernel log is not this one's) */
	print("== XNULOADER run (usb) ==\n");
}

static char klog[300 * 1024];

static void
kernel_said(void)
{
	for (unsigned i = 0; i < sizeof(klog); i++) {
		klog[i] = (char)('a' + i % 26);
	}
}

/* whether `text` is in the first `n` bytes of `p` */
static bool
holds(const uint8_t *p, uint64_t n, const char *text)
{
	size_t length = strlen(text);

	for (uint64_t i = 0; i + length <= n; i++) {
		if (memcmp(p + i, text, length) == 0) {
			return true;
		}
	}
	return false;
}

/* only logdump was written, from its first block, inside it */
static bool
only_logdump_written(void)
{
	return parts[0].writes == 0 && parts[2].writes == 0 && parts[LOGDUMP].past_end == 0;
}

/* ---- scenarios -------------------------------------------------------------------------------------------- */

/* the log and the kernel's go into logdump from its first block, flushed, and read back the same; nothing else is written */
static void
the_log_goes_into_logdump(void)
{
	phone();
	kernel_said();
	write_logs(klog, 100 * 1024);
	CHECK(parts[LOGDUMP].writes == 1 && parts[LOGDUMP].lba == 0 && parts[LOGDUMP].size % 4096 == 0, "one write from block 0: %u at %llu",
	    parts[LOGDUMP].writes, (unsigned long long)parts[LOGDUMP].lba);
	CHECK(only_logdump_written(), "only logdump: %u %u, %u past its end", parts[0].writes, parts[2].writes, parts[LOGDUMP].past_end);
	CHECK(parts[LOGDUMP].flushes == 1, "flushed: %u", parts[LOGDUMP].flushes);
	CHECK(holds(parts[LOGDUMP].bytes, parts[LOGDUMP].size, "== KERNEL LOG ==") && holds(parts[LOGDUMP].bytes, parts[LOGDUMP].size, "XNULOADER run"),
	    "the loader's log and the kernel's");
	CHECK(strstr(said, "read back the same") != NULL, "and that is said: %s", said);
	CHECK(handles_given == 0, "the handles' list is given back");
}

/* no logdump: nothing is written anywhere (the kernel's log goes onto the console) */
static void
no_logdump(void)
{
	phone();
	parts[LOGDUMP].info.name[7] = 'x';      /* "logdumpx" */
	kernel_said();
	write_logs(klog, 1000);
	CHECK(parts[0].writes == 0 && parts[LOGDUMP].writes == 0 && parts[2].writes == 0, "nothing written: %u %u %u", parts[0].writes,
	    parts[LOGDUMP].writes, parts[2].writes);
	CHECK(strstr(said, "== KERNEL LOG ==") != NULL, "the kernel's log on the console instead");
}

/* a logdump smaller than the log: nothing written past its end -- and a medium that gives no block size is not divided by */
static void
larger_than_the_partition(void)
{
	phone();
	parts[LOGDUMP].media.last_block = 15;   /* 64 KB */
	kernel_said();
	write_logs(klog, 200 * 1024);
	CHECK(parts[LOGDUMP].past_end == 0, "%u writes past the partition's end (the last of %llu bytes)", parts[LOGDUMP].past_end,
	    (unsigned long long)parts[LOGDUMP].size);
	CHECK(only_logdump_written(), "only logdump: %u %u", parts[0].writes, parts[2].writes);
	CHECK(strstr(said, "NOT WRITTEN") != NULL, "and that is said: %s", said);
	/* a log that fits is written */
	phone();
	parts[LOGDUMP].media.last_block = 15;
	write_logs(NULL, 0);
	CHECK(parts[LOGDUMP].writes == 1 && parts[LOGDUMP].past_end == 0 && strstr(said, "read back the same") != NULL, "a log that fits: %s",
	    said);
	phone();
	parts[LOGDUMP].media.block_size = 0;
	write_logs(NULL, 0);
	CHECK(parts[LOGDUMP].writes == 0 && strstr(said, "NOT WRITTEN") != NULL, "no block size: %s", said);
}

/* a flush that fails: the blocks may be in the device's cache only, and a read back from there says nothing */
static void
flush_that_fails(void)
{
	phone();
	flush_status = EFI_ERROR_BIT | 7;       /* EFI_DEVICE_ERROR */
	write_logs(NULL, 0);
	CHECK(parts[LOGDUMP].writes == 1 && parts[LOGDUMP].flushes == 1, "written and flushed: %u, %u", parts[LOGDUMP].writes,
	    parts[LOGDUMP].flushes);
	CHECK(strstr(said, "read back the same") == NULL, "not said to be the same: %s", said);
	CHECK(strstr(said, "flush FAILED") != NULL, "the flush said to have failed: %s", said);
}

/* the kernel's own transfer list in the controller: nothing is written (and that is said) */
static void
only_through_uefis_list(void)
{
	phone();
	kernels_list_in = true;
	write_logs(NULL, 0);
	CHECK(parts[0].writes == 0 && parts[LOGDUMP].writes == 0 && parts[2].writes == 0, "nothing written");
	CHECK(strstr(said, "NOT WRITTEN") != NULL, "and that is said: %s", said);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "the_log_goes_into_logdump", the_log_goes_into_logdump }, { "no_logdump", no_logdump },
		{ "larger_than_the_partition", larger_than_the_partition }, { "flush_that_fails", flush_that_fails },
		{ "only_through_uefis_list", only_through_uefis_list },
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
