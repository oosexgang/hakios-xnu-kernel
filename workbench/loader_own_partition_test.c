/*
 * The loader's answer to "is this the one partition a write may reach?" (arch/arm64/boot/src/own_partition.c),
 * compiled as it is into a host program, with the boards the tests' tables describe. A wrong yes here is
 * a write into somebody's partition; so every field of the answer has a scenario in which it alone is
 * off, and so has a board that names no such partition at all.
 *
 *   loader_own_partition_test SCENARIO
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efi.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

static char said[4096];
/* (the loader's own print: %s takes text, %d and %x a number of 64 bits) */
static void
print(const char *format, ...)
{
	va_list list;
	size_t at = strlen(said);

	va_start(list, format);
	for (const char *c = format; *c != 0 && at + 40 < sizeof(said); c++) {
		if (c[0] == '%' && c[1] == 's') {
			at += (size_t)snprintf(said + at, sizeof(said) - at, "%s", va_arg(list, const char *));
			c++;
		} else if (c[0] == '%' && (c[1] == 'd' || c[1] == 'x')) {
			at += (size_t)snprintf(said + at, sizeof(said) - at, c[1] == 'd' ? "%llu" : "%llx", (unsigned long long)va_arg(list, uint64_t));
			c++;
		} else {
			said[at++] = *c;
			said[at] = 0;
		}
	}
	va_end(list);
}

#include "src/platform.h"                       /* struct platform, and the tests' boards (platforms.h) */
static const struct platform *PLAT;
#include "src/block_page.h"
#include "src/own_partition.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- a partition as the firmware reports one, and as the loader's table has it ------------------------ */

static efi_partition_info info;
static efi_block_media media;
static efi_block_io io = { .media = &media };
static struct loader_block_part part;

/* the partition the board in `PLAT' names, exactly */
static void
the_boards_own(void)
{
	memset(&info, 0, sizeof(info));
	memset(&part, 0, sizeof(part));
	memset(&media, 0, sizeof(media));
	info.type = 2;
	info.type_guid = PLAT->own.type;
	info.unique_guid = PLAT->own.unique;
	info.start_lba = PLAT->own.first_lba;
	info.end_lba = PLAT->own.last_lba;
	strcpy(part.name, "darwin");
	part.block_size = 4096;
	part.blocks = PLAT->own.last_lba - PLAT->own.first_lba + 1;
	part.start_lba = PLAT->own.first_lba;
	part.unit = 0;
	said[0] = 0;
}

static int
asked(void)
{
	said[0] = 0;
	return block_is_darwin(&info, &io, &part);
}

/* one thing off: no, and why; put right again: yes */
#define OFF(change, undo, why) do { change; CHECK(asked() == 0 && strstr(said, "no write will be served") != NULL, "%s: %s", why, said); \
	undo; CHECK(asked() == 1, "(and right again it is the partition)"); } while (0)

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the partition the board names, as it was when the port was set up: yes, and from which block is said */
static void
the_partition(void)
{
	PLAT = &platform_phone_video;
	the_boards_own();
	CHECK(asked() == 1 && strstr(said, "darwin (LBA 25374720..30617594 of unit 0): its blocks from 2042368 on may be written this run") != NULL,
	    "it is the one: %s", said);
	CHECK(BLOCK_WRITE_FIRST == 2042368 && BLOCK_DARWIN_FIRST == 25374720 && BLOCK_DARWIN_LAST == 30617594,
	    "the numbers that were in the loader's code: %llu, %llu, %llu", (unsigned long long)BLOCK_WRITE_FIRST,
	    (unsigned long long)BLOCK_DARWIN_FIRST, (unsigned long long)BLOCK_DARWIN_LAST);
	{
		static const efi_guid type = { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } };
		static const efi_guid unique = { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } };

		CHECK(memcmp(&PLAT->own.type, &type, sizeof(type)) == 0 && memcmp(&PLAT->own.unique, &unique, sizeof(unique)) == 0,
		    "and the two GUIDs that were there");
	}
}

/* every field of the answer, alone off by the least there is */
static void
one_thing_off(void)
{
	PLAT = &platform_phone_video;
	the_boards_own();
	CHECK(asked() == 1, "(the partition as it is)");
	OFF(info.type_guid.d[7] ^= 1, info.type_guid.d[7] ^= 1, "the last byte of its type GUID");
	OFF(info.type_guid.a ^= 0x80000000u, info.type_guid.a ^= 0x80000000u, "the first bit of its type GUID");
	OFF(info.unique_guid.c ^= 1, info.unique_guid.c ^= 1, "its unique GUID");
	OFF(info.start_lba++, info.start_lba--, "it begins a block later");
	OFF(info.start_lba--, info.start_lba++, "it begins a block earlier");
	OFF(info.end_lba++, info.end_lba--, "it ends a block later");
	OFF(info.end_lba--, info.end_lba++, "it ends a block earlier");
	OFF(part.unit = 1, part.unit = 0, "it is on the second logical unit");
	OFF(part.unit = 0xffff, part.unit = 0, "its logical unit is not known");
	OFF(part.block_size = 512, part.block_size = 4096, "its blocks are of another size");
	OFF(part.blocks++, part.blocks--, "it has a block more");
	OFF(part.blocks--, part.blocks++, "it has a block less");
	OFF(media.read_only = 1, media.read_only = 0, "the firmware gives it out read only");
}

/* a partition of another name is not asked about at all: nothing is said of it */
static void
another_name(void)
{
	static const char *const names[] = { "darwin2", "darwi", "Darwin", "userdata", "", "xdarwin" };

	PLAT = &platform_phone_video;
	the_boards_own();
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		memset(part.name, 0, sizeof(part.name));
		strcpy(part.name, names[i]);
		CHECK(asked() == 0 && said[0] == 0, "\"%s\" is not it, and nothing is said: %s", names[i], said);
	}
}

/*
 * A board whose file names no such partition: no partition is it -- not one called darwin, and not one
 * whose entry is all zeros, as the description of nothing is.
 */
static void
board_names_none(void)
{
	PLAT = &platform_qemu_virt;
	the_boards_own();                       /* (a partition that is exactly what "nothing" describes) */
	CHECK(asked() == 0 && strstr(said, "does not say which partition that is (storage { })") != NULL, "no: %s", said);
	PLAT = &platform_phone_video;
	the_boards_own();
	PLAT = &platform_qemu_virt;
	CHECK(asked() == 0, "nor another board's partition");
}

/* a board with a partition of its own elsewhere: its own is it, the first phone's is not */
static void
another_board(void)
{
	static const struct platform other = {
		.name = "other",
		.own = { .type = { 0x11111111, 0x2222, 0x3333, { 1, 2, 3, 4, 5, 6, 7, 8 } }, .unique = { 0x99999999, 0x8888, 0x7777, { 8, 7, 6, 5, 4, 3, 2, 1 } },
			 .first_lba = 1000000, .last_lba = 1999999, .write_first = 4096 },
	};

	PLAT = &other;
	the_boards_own();
	CHECK(asked() == 1 && BLOCK_WRITE_FIRST == 4096, "its own partition, from its own block: %s", said);
	PLAT = &platform_phone_video;
	the_boards_own();
	PLAT = &other;
	CHECK(asked() == 0, "the first phone's partition is not this board's");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "the_partition", the_partition }, { "one_thing_off", one_thing_off }, { "another_name", another_name },
		{ "board_names_none", board_names_none }, { "another_board", another_board },
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
