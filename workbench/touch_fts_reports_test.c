/*
 * What the FT3658U driver (drivers/board/fts.c, its protocol in board/fts_protocol.h) makes of
 * the controller's reports, compiled as it is into a host program. A report is decoded whole before any
 * finger moves: one of the wrong length, saying more fingers than there are, naming a finger twice, with an
 * event that is none, a position off the panel, or a count that is not its fingers' is refused and changes
 * nothing; the first slot without a finger's number ends it; a finger lifted needs no position. Decoded,
 * fingers become touch-downs, moves and lifts, positions scaled to 0..65535 and mirrored as the board asks.
 * And the report's CRC-16 (reflected 0x1021 from 0xffff), and a report of all 0xff that is no report.
 *
 *   touch_fts_reports_test SCENARIO
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
#include "board/fts_protocol.h"      /* (by its name here: fts.c's own include of it is then the same file) */
#include "board/fts.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t report[FTS_REPORT_BYTES];

/* an empty report (every slot 0xff), saying `count' fingers */
static void
blank(unsigned count)
{
	memset(report, 0xff, sizeof(report));
	report[0] = 0;
	report[1] = (uint8_t)count;
}

/* slot `slot': finger `id', its event (0 down, 1 up, 2 contact) and position */
static void
point(unsigned slot, unsigned id, unsigned event, unsigned x, unsigned y)
{
	uint8_t *p = report + 2 + 6 * slot;

	p[0] = (uint8_t)(event << 6 | (x >> 8 & 0xf));
	p[1] = (uint8_t)x;
	p[2] = (uint8_t)(id << 4 | (y >> 8 & 0xf));
	p[3] = (uint8_t)y;
	p[4] = p[5] = 0;
}

static bool
decodes(struct fts_frame *frame)
{
	return fts_decode(report, sizeof(report), FTS_MAX_X, FTS_MAX_Y, frame);
}

/* two fingers, one lifted by its event (no position needed), and the slot after the last ends the report */
static void
decoded(void)
{
	struct fts_frame frame;

	blank(2);
	point(0, 3, 0, 540, 1200);
	point(1, 9, 2, FTS_MAX_X, FTS_MAX_Y);
	point(2, 5, 1, 0xfff, 0xfff);
	CHECK(decodes(&frame) && frame.active == (1u << 3 | 1u << 9), "fingers 3 and 9: %#x", frame.active);
	CHECK(frame.x[3] == 540 && frame.y[3] == 1200 && frame.x[9] == FTS_MAX_X && frame.y[9] == FTS_MAX_Y, "where they are");
	blank(0);
	point(0, 15, 0, 1, 1);
	point(1, 2, 0, 1, 1);
	CHECK(decodes(&frame) && frame.active == 0, "a slot numbered 15 ends it: the one after is not read");
	blank(1);
	point(0, 15, 0, 1, 1);
	point(1, 2, 0, 1, 1);
	CHECK(!decodes(&frame), "and then a count of one is not its fingers'");
	blank(0);
	point(0, 10, 0, 1, 1);
	point(1, 2, 0, 1, 1);
	CHECK(decodes(&frame) && frame.active == 0, "so does one numbered 10, the first there is not");
}

/* each of these refused, the frame given left as it was */
static void
refused(void)
{
	struct fts_frame frame = { .active = 0x1234 };

	blank(1);
	point(0, 1, 0, 10, 10);
	CHECK(!fts_decode(report, sizeof(report) - 1, FTS_MAX_X, FTS_MAX_Y, &frame), "a report of the wrong length");
	blank(11);
	CHECK(!decodes(&frame), "more fingers than there are");
	blank(2);
	point(0, 1, 0, 10, 10);
	point(1, 1, 0, 20, 20);
	CHECK(!decodes(&frame), "one finger twice");
	blank(1);
	point(0, 1, 3, 10, 10);
	CHECK(!decodes(&frame), "an event that is none");
	blank(1);
	point(0, 1, 0, FTS_MAX_X + 1, 10);
	CHECK(!decodes(&frame), "off the panel's side");
	blank(1);
	point(0, 1, 0, 10, FTS_MAX_Y + 1);
	CHECK(!decodes(&frame), "off its end");
	blank(2);
	point(0, 1, 0, 10, 10);
	CHECK(!decodes(&frame), "a count that is not its fingers'");
	CHECK(frame.active == 0x1234, "and the frame given untouched");
	blank(10);
	for (unsigned i = 0; i < 10; i++) {
		point(i, i, 2, i, i);
	}
	CHECK(decodes(&frame) && frame.active == 0x3ff, "ten fingers, as many as there are");
}

/* decoded fingers as touches: down, moved either way, held, lifted once; scaled, and mirrored as asked */
static void
touches(void)
{
	struct fts_frame frame = { 0 };

	frame.active = 1u << 4;
	frame.x[4] = FTS_MAX_X / 2;
	frame.y[4] = FTS_MAX_Y;
	fts_report(&frame);
	CHECK(posts == 1 && posted[0].type == HAKIOS_TOUCH_DOWN && posted[0].finger == 4 && posted[0].x == 32767 &&
	    posted[0].y == 65535, "down: %u %u", posted[0].x, posted[0].y);
	fts_report(&frame);
	CHECK(posts == 1, "held: nothing");
	frame.y[4] = 0;
	fts_report(&frame);
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_MOVE && posted[1].y == 0, "moved up");
	frame.x[4] = 0;
	fts_report(&frame);
	CHECK(posts == 3 && posted[2].type == HAKIOS_TOUCH_MOVE && posted[2].x == 0, "moved left");
	fts_release();
	CHECK(posts == 4 && posted[3].type == HAKIOS_TOUCH_UP && posted[3].finger == 4 && posted[3].x == 0, "lifted where it was");
	fts_release();
	CHECK(posts == 4, "once");
	flip = 3;
	frame.x[4] = 0;
	frame.y[4] = FTS_MAX_Y;
	fts_report(&frame);
	CHECK(posts == 5 && posted[4].x == 65535 && posted[4].y == 0, "mirrored both ways: %u %u", posted[4].x, posted[4].y);
}

/* the CRC: 0x6f91 for "123456789"; and all 0xff, of the report's length only, is no report */
static void
crc_and_empty(void)
{
	CHECK(fts_crc((const uint8_t *)"123456789", 9) == 0x6f91, "%#x", fts_crc((const uint8_t *)"123456789", 9));
	memset(report, 0xff, sizeof(report));
	CHECK(fts_report_empty(report, sizeof(report)), "all 0xff");
	CHECK(!fts_report_empty(report, sizeof(report) - 1), "not of the report's length");
	report[sizeof(report) - 1] = 0xfe;
	CHECK(!fts_report_empty(report, sizeof(report)), "its last byte counts");
}

/* all 0xff with a finger down (its lift missed): decoded as no finger, and the finger lifted */
static void
idle_lifts(void)
{
	struct fts_frame frame = { 0 };

	frame.active = 1u << 2;
	frame.x[2] = 100;
	frame.y[2] = 200;
	fts_report(&frame);
	CHECK(posts == 1 && posted[0].type == HAKIOS_TOUCH_DOWN, "down");
	memset(report, 0xff, sizeof(report));
	frame.active = 0x1234;
	CHECK(decodes(&frame) && frame.active == 0, "all 0xff is a frame of no finger: %#x", frame.active);
	fts_report(&frame);
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 2, "and the finger lifted");
	CHECK(!fts_decode(report, sizeof(report) - 1, FTS_MAX_X, FTS_MAX_Y, &frame), "not of the report's length: refused");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "decoded", decoded }, { "refused", refused }, { "touches", touches }, { "crc_and_empty", crc_and_empty },
		{ "idle_lifts", idle_lifts },
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
			board = "phone-keys";
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
