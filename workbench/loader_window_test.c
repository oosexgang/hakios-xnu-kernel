/*
 * The loader's choice of the kernel's memory (arch/arm64/boot/src/window.c), compiled as it is into a host
 * program and given memory maps a firmware might report. The kernel gets one block of RAM, the
 * largest free one, clear of the payload that sits in memory the firmware calls free; the loader
 * claims it from the firmware, which stays resident. A phone with another memory map is the first
 * thing a port meets: this is where its map can be tried.
 *
 *   loader_window_test SCENARIO      one scenario a process: the window is chosen once
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efi.h"

#define memset loader_memset
#define memcpy loader_memcpy
#define memcmp loader_memcmp
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

static char said[8192];
static void
print(const char *format, ...)
{
	snprintf(said + strlen(said), sizeof(said) - strlen(said), "%s", format);
}

#include "src/conventions.h"
#include "src/xnu.h"
#include "src/base.c"
#include "src/payload.c"
#include "src/window.c"

#undef memset
#undef memcpy
#undef memcmp

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define MB              (1ULL << 20)
#define GB              (1ULL << 30)

/* ---- a firmware -------------------------------------------------------------------------------- */

static struct { uint32_t type; uint64_t from, to; } ranges[16];
static unsigned range_count, descriptor_size = sizeof(efi_memory_descriptor);
static unsigned maps_asked;
static efi_status map_status, claim_status, low_status;
static struct { int how, kind; uint64_t pages, at; } claims[4];
static unsigned claim_count;

static void
range(uint32_t type, uint64_t from, uint64_t to)
{
	ranges[range_count].type = type;
	ranges[range_count].from = from;
	ranges[range_count++].to = to;
}

static efi_status EFIAPI
get_memory_map(uint64_t *size, void *map, uint64_t *key, uint64_t *desc_size, uint32_t *desc_ver)
{
	maps_asked++;
	if (EFI_ERROR(map_status)) {
		return map_status;
	}
	if (*size < range_count * descriptor_size) {
		return EFI_BUFFER_TOO_SMALL;
	}
	memset(map, 0xff, range_count * descriptor_size);      /* (what a larger descriptor has after the fields known) */
	for (unsigned i = 0; i < range_count; i++) {
		efi_memory_descriptor d = { .type = ranges[i].type, .phys_start = ranges[i].from, .pages = (ranges[i].to - ranges[i].from) >> 12 };

		memcpy((uint8_t *)map + i * descriptor_size, &d, sizeof(d));
	}
	*size = range_count * descriptor_size;
	*desc_size = descriptor_size;
	*desc_ver = 1;
	*key = 7;
	return 0;
}

static efi_status EFIAPI
allocate_pages(int how, int kind, uint64_t pages, uint64_t *at)
{
	if (claim_count < 4) {
		claims[claim_count].how = how;
		claims[claim_count].kind = kind;
		claims[claim_count].pages = pages;
		claims[claim_count].at = *at;
	}
	claim_count++;
	if (how == AllocateMaxAddress) {
		if (!EFI_ERROR(low_status)) {
			*at = 0x90000000ULL;            /* somewhere under the limit it gave */
		}
		return low_status;
	}
	return claim_status;            /* (also for a claim of nothing: firmwares differ) */
}

static efi_boot_services services = { .get_memory_map = get_memory_map, .allocate_pages = allocate_pages };
static struct payload_info payload;

static int
choose(void)
{
	BS = &services;
	return choose_window(&payload);
}

/* what holds for every window that is chosen: whole blocks of 32 MB in one free range, clear of the payload, claimed as it is */
static void
sound(void)
{
	bool inside = false;

	CHECK(win_size != 0 && (win_base & (BASE_ALIGN - 1)) == 0 && (win_size & (BASE_ALIGN - 1)) == 0, "the window %#llx + %#llx is whole blocks of 32 MB",
	    (unsigned long long)win_base, (unsigned long long)win_size);
	CHECK(win_size <= MAX_MEM_SIZE, "no more than the kernel's first page tables cover: %#llx", (unsigned long long)win_size);
	CHECK(win_base + win_size <= PAYLOAD_BASE || win_base >= PAYLOAD_BASE + PAYLOAD_MAX_SIZE, "the window is clear of the payload");
	for (unsigned i = 0; i < range_count; i++) {
		inside |= ranges[i].type == EfiConventionalMemory && win_base >= ranges[i].from && win_base + win_size <= ranges[i].to;
	}
	CHECK(inside || payload.forced_base != 0, "the window lies in one range the firmware calls free");
	CHECK(claims[0].how == AllocateAddress && claims[0].kind == EfiLoaderData && claims[0].at == win_base && claims[0].pages == win_size >> 12,
	    "the firmware was asked for exactly the window: %#llx, %llu pages", (unsigned long long)claims[0].at, (unsigned long long)claims[0].pages);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* a phone of 8 GB: the payload lies in the largest free range, nearer its start -- the window is what follows it */
static void
phone_8gb(void)
{
	range(EfiConventionalMemory, 0x80000000ULL, 0x9fe00000ULL);
	range(0, 0x9fe00000ULL, 0xc0000000ULL);                 /* the firmware's own, the other processors' */
	range(EfiConventionalMemory, 0xc0000000ULL, 0x27fe00000ULL);
	range(4, 0x27fe00000ULL, 0x280000000ULL);               /* boot services' data at the top */
	CHECK(choose() == 1, "a window is found");
	CHECK(win_base == 0xf4000000ULL && win_size == 0x27e000000ULL - 0xf4000000ULL, "from the payload's end to the last whole 32 MB: %#llx + %#llx",
	    (unsigned long long)win_base, (unsigned long long)win_size);
	sound();
	CHECK(claim_count == 2 && claims[1].how == AllocateMaxAddress && claims[1].at == 0xffffffffULL && claims[1].pages == LOW_DMA_SIZE >> 12,
	    "memory under 4 GB is asked for as well, once");
	CHECK(low_dma == 0x90000000ULL, "and kept: %#llx", (unsigned long long)low_dma);
	/* asked again (the next run of a kernel): the same window, the firmware is not asked */
	CHECK(choose() == 1 && maps_asked == 1 && claim_count == 2, "the window is chosen once");
}

/* the payload nearer the range's end: the window is what precedes it */
static void
payload_near_the_end(void)
{
	range(EfiConventionalMemory, 0x80000000ULL, 0xf8000000ULL);
	CHECK(choose() == 1 && win_base == 0x80000000ULL && win_size == 0x70000000ULL, "up to the payload: %#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/* a free range that ends where the payload begins is not in it: taken whole */
static void
up_to_the_payload(void)
{
	range(EfiConventionalMemory, 0xc0000000ULL, PAYLOAD_BASE);
	CHECK(choose() == 1 && win_base == 0xc0000000ULL && win_size == PAYLOAD_BASE - 0xc0000000ULL, "%#llx + %#llx",
	    (unsigned long long)win_base, (unsigned long long)win_size);
	sound();
}

/* the payload in the middle of a range, as much before it as after: the window is what follows it */
static void
payload_in_the_middle(void)
{
	range(EfiConventionalMemory, PAYLOAD_BASE - 0x10000000ULL, PAYLOAD_BASE + PAYLOAD_MAX_SIZE + 0x10000000ULL);
	CHECK(choose() == 1 && win_base == PAYLOAD_BASE + PAYLOAD_MAX_SIZE && win_size == 0x10000000ULL, "after it: %#llx + %#llx",
	    (unsigned long long)win_base, (unsigned long long)win_size);
	sound();
}

/* a free block that ends inside the payload's place: what is below it kept, not the whole block dropped */
static void
ends_in_the_payload(void)
{
	range(EfiConventionalMemory, PAYLOAD_BASE - 0x8000000ULL, PAYLOAD_BASE + 0x1000000ULL);
	CHECK(choose() == 1 && win_base == PAYLOAD_BASE - 0x8000000ULL && win_size == 0x8000000ULL, "below it: %#llx + %#llx",
	    (unsigned long long)win_base, (unsigned long long)win_size);
}

/* how many times `text' is in what was said */
static unsigned
said_times(const char *text)
{
	unsigned n = 0;

	for (const char *at = said; (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

/* the memory map said: each range of 1 MB or more (not one less), and a total for each type there is of the sixteen */
static void
memory_map_said(void)
{
	range(EfiConventionalMemory, 0x80000000ULL, 0x80100000ULL);    /* 256 pages: said */
	range(0, 0x80100000ULL, 0x801ff000ULL);                         /* 255: not */
	range(16, 0x80200000ULL, 0x80400000ULL);                        /* a type past the sixteen: said, in no total */
	BS = &services;
	print_memory_map();
	CHECK(said_times("  %x - %x  type") == 2, "two ranges said: %u", said_times("  %x - %x  type"));
	CHECK(said_times("  total of type") == 2, "two totals, of types 0 and 7: %u", said_times("  total of type"));
}

/* a free range the payload is not in is taken whole; ends that are not on 32 MB are cut to it */
static void
ends_are_cut(void)
{
	range(EfiConventionalMemory, 0x100001000ULL, 0x17ffff000ULL);
	range(EfiConventionalMemory, 0x40000000ULL, 0x48000000ULL);
	CHECK(choose() == 1 && win_base == 0x102000000ULL && win_size == 0x17e000000ULL - 0x102000000ULL, "%#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/* more RAM than the kernel's first page tables cover: the window is that much, from the range's start */
static void
more_than_the_kernel_takes(void)
{
	range(EfiConventionalMemory, 0x100000000ULL, 0x400000000ULL);  /* 12 GB */
	CHECK(choose() == 1 && win_base == 0x100000000ULL && win_size == MAX_MEM_SIZE, "%#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/* the largest of several, whatever their order; memory that is not free does not count however large */
static void
largest_free_range(void)
{
	range(EfiConventionalMemory, 0x40000000ULL, 0x60000000ULL);
	range(3, 0x100000000ULL, 0x300000000ULL);               /* boot services' code: not free */
	range(EfiConventionalMemory, 0x300000000ULL, 0x340000000ULL);
	range(EfiConventionalMemory, 0x60000000ULL, 0x70000000ULL);
	CHECK(choose() == 1 && win_base == 0x300000000ULL && win_size == 1 * GB, "%#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/* a firmware whose descriptors are longer than the fields known here (they are: the map says how long) */
static void
longer_descriptors(void)
{
	descriptor_size = sizeof(efi_memory_descriptor) + 8;
	range(EfiConventionalMemory, 0x40000000ULL, 0x60000000ULL);
	range(EfiConventionalMemory, 0x100000000ULL, 0x180000000ULL);
	range(EfiConventionalMemory, 0x60000000ULL, 0x70000000ULL);
	CHECK(choose() == 1 && win_base == 0x100000000ULL && win_size == 2 * GB, "%#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/* no free range of 32 MB anywhere: no window, and the kernel is not run */
static void
nothing_free(void)
{
	range(4, 0x80000000ULL, 0x280000000ULL);
	range(EfiConventionalMemory, 0x40001000ULL, 0x41fff000ULL);     /* (31.99 MB) */
	CHECK(choose() == 0 && win_size == 0, "no window: %#llx + %#llx", (unsigned long long)win_base, (unsigned long long)win_size);
	CHECK(claim_count == 0, "the firmware is not asked for a window of nothing");
	CHECK(strstr(said, "no free range of 32 MB") != NULL, "and why is said: %s", said);
}

/* the firmware refuses the claim (the range is not free after all), or has no map: no window */
static void
firmware_says_no(void)
{
	range(EfiConventionalMemory, 0x100000000ULL, 0x180000000ULL);
	claim_status = EFI_ERROR_BIT | 14;
	CHECK(choose() == 0 && win_size == 0, "a claim that is refused is no window");
	CHECK(claim_count == 1, "and no other memory is asked for after it");
	claim_status = 0;
	map_status = EFI_ERROR_BIT | 2;
	CHECK(choose() == 0 && win_size == 0 && strstr(said, "GetMemoryMap failed") != NULL, "nor is there one without a map: %s", said);
	map_status = 0;
	CHECK(choose() == 1 && win_base == 0x100000000ULL, "once both work, there is");
}

/* no memory under 4 GB to be had: the window stands, the kernel is told there is none */
static void
no_low_memory(void)
{
	range(EfiConventionalMemory, 0x100000000ULL, 0x180000000ULL);
	low_status = EFI_ERROR_BIT | 9;
	CHECK(choose() == 1 && win_size == 2 * GB && low_dma == 0, "the window without it: low memory at %#llx", (unsigned long long)low_dma);
	sound();
}

/* "physbase" in the payload (a testing aid): the window is put there, whatever the map says */
static void
forced_base(void)
{
	range(EfiConventionalMemory, 0x100000000ULL, 0x180000000ULL);
	payload.forced_base = 0x200000000ULL;
	CHECK(choose() == 1 && win_base == 0x200000000ULL && win_size == MAX_MEM_SIZE, "%#llx + %#llx", (unsigned long long)win_base,
	    (unsigned long long)win_size);
	sound();
}

/*
 * "getvar result": the last command's line, formatted apart from the log -- a run leaves the log full, and a line taken
 * from it came back empty ("result: "), which bench took for a run that had ended -- and at most 63 letters of it
 */
static void
result_line(void)
{
	static char what[100];

	log_len = 5;
	set_result("running", 1, 2);
	CHECK(strcmp(last_result, "running 1 2") == 0 && log_len == 5, "\"%s\", log %u", last_result, log_len);
	log_len = sizeof(log_buf) - 1;          /* (full: what print() would put there is dropped) */
	set_result("hung", 1234, 0xabc);
	CHECK(strcmp(last_result, "hung 1234 abc") == 0, "with the log full: \"%s\"", last_result);
	CHECK(log_len == sizeof(log_buf) - 1, "and the log is as it was: %u", log_len);
	for (size_t length = 1; length < sizeof(what); length++) {
		size_t kept = length + 4 < 63 ? length + 4 : 63;        /* (" 0 0" after it) */

		memset(what, 'a', length);
		what[length] = 0;
		memset(last_result, '#', sizeof(last_result));
		set_result(what, 0, 0);
		CHECK(strlen(last_result) == kept && memcmp(last_result, what, kept < length ? kept : length) == 0,
		    "a word of %zu letters: %zu kept, %zu are", length, kept, strlen(last_result));
	}
}

/* "getvar screen", and every line format_into() makes: the loader's %s %d %x of 64-bit values, cut to fit and ended */
static void
format_lines(void)
{
	char text[16];

	format_into(text, sizeof(text), "%d %d %d", (uint64_t)1080, (uint64_t)2400, (uint64_t)4352);
	CHECK(strcmp(text, "1080 2400 4352") == 0, "\"%s\"", text);
	format_into(text, sizeof(text), "%x|%s|%%|%d", (uint64_t)0xdeadbeefULL, "ab", (uint64_t)0);
	CHECK(strcmp(text, "deadbeef|ab|%|0") == 0, "\"%s\"", text);
	format_into(text, sizeof(text), "%d", (uint64_t)18446744073709551615ULL);
	CHECK(strcmp(text, "184467440737095") == 0, "cut to 15 letters and ended: \"%s\"", text);
	format_into(text, 1, "%s", "anything");
	CHECK(text[0] == 0, "no room but for its end");
	memset(text, '#', sizeof(text));
	format_into(text, 0, "%s", "anything");
	CHECK(text[0] == '#', "no room at all: nothing written");
	format_into(text, sizeof(text), "ab%");
	CHECK(strcmp(text, "ab") == 0, "a '%%' at the end: nothing read after it: \"%s\"", text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "result_line", result_line }, { "format_lines", format_lines },
		{ "phone_8gb", phone_8gb }, { "payload_near_the_end", payload_near_the_end }, { "ends_are_cut", ends_are_cut },
		{ "more_than_the_kernel_takes", more_than_the_kernel_takes }, { "largest_free_range", largest_free_range },
		{ "longer_descriptors", longer_descriptors }, { "nothing_free", nothing_free }, { "firmware_says_no", firmware_says_no },
		{ "no_low_memory", no_low_memory }, { "forced_base", forced_base }, { "up_to_the_payload", up_to_the_payload },
		{ "payload_in_the_middle", payload_in_the_middle }, { "ends_in_the_payload", ends_in_the_payload }, { "memory_map_said", memory_map_said },
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
