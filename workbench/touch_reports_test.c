/*
 * What the Goodix and FocalTech driver (drivers/board/goodix.c) makes of the touches the
 * controller reports, compiled as it is into a host program: a report's fingers become touch-downs, moves
 * and lifts, each finger by its number; positions are scaled to 0..65535 and mirrored when the board asks;
 * a stylus and a finger number out of range are left out; a lifted finger is lifted once. And the checks
 * the driver puts the controller's answers through: Goodix's 16-bit sum, where a block of all zeros or all
 * ones is no answer, and FocalTech's CRC-16 (reflected 0x8408 from 0xffff).
 *
 *   touch_reports_test SCENARIO
 */
#include "fakekernel.h"

/* the touches posted, in order */
static struct { uint32_t type, finger, x, y; } posted[64];
static unsigned posts;

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	if (posts < 64) {
		posted[posts].type = type;
		posted[posts].finger = finger;
		posted[posts].x = x;
		posted[posts].y = y;
	}
	posts++;
}

uint32_t hakios_test_rd(vm_offset_t address __unused) { return 0; }
void hakios_test_wr(vm_offset_t address __unused, uint32_t value __unused) { }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_tl(const char *format __unused, ...) { }
void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/goodix.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the post numbered `n' is this */
static bool
was(unsigned n, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	return n < posts && posted[n].type == type && posted[n].finger == finger && posted[n].x == x && posted[n].y == y;
}

/* ---- Goodix ---------------------------------------------------------------------------------------------- */

/* a Goodix touch record: the finger's number and kind, then x and y */
static void
goodix_touch(uint8_t *t, unsigned id, unsigned kind, unsigned x, unsigned y)
{
	memset(t, 0, 8);
	t[0] = (uint8_t)(id << 4 | kind);
	t[2] = (uint8_t)x;
	t[3] = (uint8_t)(x >> 8);
	t[4] = (uint8_t)y;
	t[5] = (uint8_t)(y >> 8);
}

/* a finger down, moved, held still, joined by another, and both lifted; positions scaled to 0..65535 */
static void
goodix_fingers(void)
{
	uint8_t t[3 * 8];

	max_x = 1000;
	max_y = 2000;
	goodix_touch(t, 2, 0, 500, 1000);
	goodix_report(t, 1);
	CHECK(posts == 1 && was(0, HAKIOS_TOUCH_DOWN, 2, 32767, 32767), "down at the middle: %u %u", posted[0].x, posted[0].y);
	goodix_touch(t, 2, 0, 1000, 0);
	goodix_report(t, 1);
	CHECK(posts == 2 && was(1, HAKIOS_TOUCH_MOVE, 2, 65535, 0), "moved to the edge: %u %u", posted[1].x, posted[1].y);
	goodix_report(t, 1);
	CHECK(posts == 2, "held still: nothing more");
	goodix_touch(t, 2, 0, 1000, 1000);
	goodix_report(t, 1);
	CHECK(posts == 3 && was(2, HAKIOS_TOUCH_MOVE, 2, 65535, 32767), "moved down only: a move too");
	goodix_touch(t, 2, 0, 1000, 0);
	goodix_report(t, 1);
	posts = 2;                              /* (and back: the move forgotten, for the numbers below) */
	goodix_touch(t + 8, 9, 0, 250, 500);
	goodix_report(t, 2);
	CHECK(posts == 3 && was(2, HAKIOS_TOUCH_DOWN, 9, 16383, 16383), "another finger, the last number there is");
	goodix_report(t, 0);
	CHECK(posts == 5 && was(3, HAKIOS_TOUCH_UP, 2, 65535, 0) && was(4, HAKIOS_TOUCH_UP, 9, 16383, 16383),
	    "both lifted where they were");
	goodix_report(t, 0);
	CHECK(posts == 5, "and lifted once");
}

/* a stylus (kinds 1 and 3) and a finger numbered 10 or more are left out; mirrored as the board says */
static void
goodix_left_out_and_mirrored(void)
{
	uint8_t t[4 * 8];

	max_x = 1000;
	max_y = 1000;
	goodix_touch(t, 1, 1, 100, 100);
	goodix_touch(t + 8, 3, 3, 100, 100);
	goodix_touch(t + 16, 10, 0, 100, 100);
	goodix_touch(t + 24, 4, 2, 100, 100);           /* (kind 2: a finger too) */
	goodix_report(t, 4);
	CHECK(posts == 1 && posted[0].finger == 4, "only the finger: %u posts", posts);
	goodix_report(t, 0);
	flip = 3;
	goodix_touch(t, 0, 0, 0, 1000);
	goodix_report(t, 1);
	CHECK(posts == 3 && was(2, HAKIOS_TOUCH_DOWN, 0, 65535, 0), "mirrored both ways: %u %u", posted[2].x, posted[2].y);
	flip = 1;
	goodix_touch(t, 5, 0, 0, 1000);
	goodix_report(t, 1);
	CHECK(posts == 5 && was(3, HAKIOS_TOUCH_DOWN, 5, 65535, 65535) && was(4, HAKIOS_TOUCH_UP, 0, 65535, 0),
	    "left-right only (and the finger before lifted): %u %u", posted[3].x, posted[3].y);
}

/* the 16-bit sum of the bytes before it, low byte first; all zeros, all ones, or too short is no answer */
static void
goodix_sums(void)
{
	uint8_t block[6] = { 0x12, 0x34, 0xf0, 0x0f, 0x45, 0x01 };    /* 0x12 + 0x34 + 0xf0 + 0x0f = 0x145 */
	uint8_t zeros[4] = { 0, 0, 0, 0 }, ones[4] = { 0xff, 0xff, 0xfe, 0x01 };

	CHECK(goodix_checksum_ok(block, 6), "the sum that is there");
	block[4] = 0x46;
	CHECK(!goodix_checksum_ok(block, 6), "and not another");
	block[4] = 0x45;
	block[5] = 0x02;
	CHECK(!goodix_checksum_ok(block, 6), "its high byte counts");
	CHECK(!goodix_checksum_ok(zeros, 4), "all zeros is no answer, though it adds up");
	CHECK(!goodix_checksum_ok(ones, 4), "nor all ones");
	CHECK(!goodix_checksum_ok(block, 1) && !goodix_checksum_ok(block, 0), "too short to have a sum");
	CHECK(goodix_checksum_ok((const uint8_t[]){ 0, 0 }, 2) == false, "a sum of nothing: no answer");
}

/* ---- FocalTech ------------------------------------------------------------------------------------------- */

/* a FocalTech record: the event and x in its first bytes, the finger's number and y in the next, the low bits last */
static void
focaltech_touch(uint8_t *report, unsigned slot, unsigned event, unsigned id, unsigned x, unsigned y)
{
	uint8_t *t = report + 2 + 6 * slot;

	t[0] = (uint8_t)(event << 6 | (x >> 11 & 0x0f));
	t[1] = (uint8_t)(x >> 3);
	t[2] = (uint8_t)(id << 4 | (y >> 11 & 0x0f));
	t[3] = (uint8_t)(y >> 3);
	t[4] = (uint8_t)((x & 7) << 5 | (y & 7) << 2);
	t[5] = 0;
}

/* a report's slots: a finger down and moved, one lifted by its event and one by leaving the report; the first
 * slot without a finger's number ends it */
static void
focaltech_fingers(void)
{
	uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];
	unsigned before;

	focaltech_max_x = 10001;
	focaltech_max_y = 20001;
	memset(report, 0xff, sizeof(report));
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 3, 5000, 10000);
	focaltech_touch(report, 1, FOCALTECH_EVENT_DOWN, 7, 10000, 0);
	focaltech_report(report);
	CHECK(posts == 2 && was(0, HAKIOS_TOUCH_DOWN, 3, 32767, 32767) && was(1, HAKIOS_TOUCH_DOWN, 7, 65535, 0),
	    "two down: %u %u, %u %u", posted[0].x, posted[0].y, posted[1].x, posted[1].y);
	focaltech_touch(report, 0, FOCALTECH_EVENT_CONTACT, 3, 0, 20000);
	focaltech_touch(report, 1, FOCALTECH_EVENT_UP, 7, 10000, 0);
	focaltech_report(report);
	CHECK(posts == 4 && was(2, HAKIOS_TOUCH_MOVE, 3, 0, 65535) && was(3, HAKIOS_TOUCH_UP, 7, 65535, 0),
	    "one moved, one lifted by its event");
	memset(report, 0xff, sizeof(report));
	focaltech_report(report);
	CHECK(posts == 5 && was(4, HAKIOS_TOUCH_UP, 3, 0, 65535), "the other lifted by leaving the report");
	focaltech_report(report);
	CHECK(posts == 5, "once");
	flip = 1;
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 9, 0, 0);
	focaltech_report(report);
	CHECK(posts == 6 && was(5, HAKIOS_TOUCH_DOWN, 9, 65535, 0), "mirrored left-right: %u %u", posted[5].x, posted[5].y);
	memset(report, 0xff, sizeof(report));
	focaltech_report(report);                       /* (9 lifted) */
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 10, 0, 0);
	focaltech_touch(report, 1, FOCALTECH_EVENT_DOWN, 2, 0, 0);
	before = posts;
	focaltech_report(report);
	CHECK(posts == before, "a slot numbered 10 ends the report: the one after it is not read (%u)", posts - before);
	focaltech_max_x = 0;
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 9, 100, 100);
	focaltech_report(report);
	CHECK(posts == before, "no panel size: no position made up");
	focaltech_max_x = 1;
	focaltech_report(report);
	CHECK(posts == before, "a panel one wide: nothing divided by none");
}

/* a position past the panel's side (the chip has 15 bits): kept on the panel, mirrored or not -- not wrapped round */
static void
focaltech_past_the_side(void)
{
	uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];

	focaltech_max_x = 10001;
	focaltech_max_y = 20001;
	memset(report, 0xff, sizeof(report));
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 1, 12000, 25000);
	focaltech_report(report);
	CHECK(posts == 1 && was(0, HAKIOS_TOUCH_DOWN, 1, 65535, 65535), "at the far edges: %u %u", posted[0].x, posted[0].y);
	memset(report, 0xff, sizeof(report));
	focaltech_report(report);
	flip = 3;
	focaltech_touch(report, 0, FOCALTECH_EVENT_DOWN, 1, 12000, 25000);
	focaltech_report(report);
	CHECK(posts == 3 && was(2, HAKIOS_TOUCH_DOWN, 1, 0, 0), "mirrored: at the near edges, %u %u", posted[2].x, posted[2].y);
}

/* CRC-16 reflected 0x8408 from 0xffff, nothing xored after (CRC-16/MCRF4XX): 0x6f91 for "123456789" */
static void
focaltech_crcs(void)
{
	CHECK(focaltech_crc((const uint8_t *)"123456789", 9) == 0x6f91, "%#x", focaltech_crc((const uint8_t *)"123456789", 9));
	CHECK(focaltech_crc((const uint8_t *)"", 0) == 0xffff, "of nothing, where it starts");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "goodix_fingers", goodix_fingers }, { "goodix_left_out_and_mirrored", goodix_left_out_and_mirrored },
		{ "goodix_sums", goodix_sums }, { "focaltech_fingers", focaltech_fingers }, { "focaltech_crcs", focaltech_crcs },
		{ "focaltech_past_the_side", focaltech_past_the_side },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			board = "phone-video";
			scenarios[i].run();
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
