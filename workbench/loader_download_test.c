/*
 * Where the loader puts a download over USB, and how much it lets come (arch/arm64/boot/src/download.c), compiled
 * as it is into a host program. The host says the size; the loader writes it into its own memory as it
 * arrives. So every combination of the commands before it has to come out inside the place it names --
 * above all after "oem ramdisk patch", whose image may have gone in the meantime.
 *
 *   loader_download_test SCENARIO
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/conventions.h"
#include "src/download.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define AREA            (RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE)
#define WINDOW          (AREA + 0x40000000ull)          /* a window with 1 GiB for the resident image */

/* the payload's area: its own size, whatever the window */
static void
payload(void)
{
	uint64_t before = 9, room = 0;

	CHECK(download_room(0, 0, 0, 0, 0, &before, &room) == NULL && before == 0 && room == PAYLOAD_MAX_SIZE - 0x100000, "%llx",
	    (unsigned long long)room);
	CHECK(download_room(0, 1, 5, 7, WINDOW, &before, &room) == NULL && before == 0 && room == PAYLOAD_MAX_SIZE - 0x100000,
	    "an append or a patch asked for does not change it");
}

/* the resident image: from the start of its area, or after what it holds */
static void
resident(void)
{
	uint64_t before = 9, room = 0;

	CHECK(download_room(1, 0, 0, 0x1000, WINDOW, &before, &room) == NULL && before == 0 && room == 0x40000000ull, "a new image: the whole area");
	CHECK(download_room(1, 1, 0, 0x1000, WINDOW, &before, &room) == NULL && before == 0x1000 && room == 0x40000000ull - 0x1000,
	    "appended: after the 0x1000 there, the rest of the area");
	CHECK(download_room(1, 1, 0, 0x40000000ull, WINDOW, &before, &room) != NULL, "appended to an image that fills the area: no room");
	CHECK(download_room(1, 1, 0, ~0ull, WINDOW, &before, &room) != NULL, "appended to an image larger than the area: no room");
	CHECK(download_room(1, 0, 0, 0, AREA, &before, &room) != NULL && room == 0, "a window no larger than what the kernel keeps: none");
	CHECK(download_room(1, 0, 0, 0, 0x1000, &before, &room) != NULL && room == 0, "a window smaller than that: none, not a wrapped size");
}

/* a patch: inside the image there is, from where it was asked for */
static void
patch(void)
{
	uint64_t before = 9, room = 0;

	CHECK(download_room(2, 0, 0x100, 0x1000, WINDOW, &before, &room) == NULL && before == 0x100 && room == 0xf00, "the rest of the image");
	CHECK(download_room(2, 0, 0xfff, 0x1000, WINDOW, &before, &room) == NULL && room == 1, "its last byte");
	CHECK(download_room(2, 0, 0x1000, 0x1000, WINDOW, &before, &room) != NULL && room == 0, "at its end: nothing");
	/* "oem ramdisk patch 100", then "oem ramdisk none": the image is gone, the patch must not wrap to everything */
	CHECK(download_room(2, 0, 0x100, 0, WINDOW, &before, &room) != NULL && room == 0, "with no image: refused, no room");
	/* ... or a smaller image read since ("oem dsu") */
	CHECK(download_room(2, 0, 0x2000, 0x1000, WINDOW, &before, &room) != NULL && room == 0, "past a smaller image: refused");
	CHECK(download_room(2, 0, ~0ull, 0x1000, WINDOW, &before, &room) != NULL, "an offset near 2^64: refused");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "payload", payload }, { "resident", resident }, { "patch", patch },
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
			fprintf(stderr, "%-12s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
