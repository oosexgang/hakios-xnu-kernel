/*
 * How the Goodix driver (drivers/board/goodix.c) reads a report: the header first with two fingers'
 * room, the rest only when there are more, the chip told it has been read, the header's and the
 * touches' checksums checked before a finger is believed, and a request of the chip's to be reset
 * honoured. The driver is compiled as it is into a host program, its SPI transfer replaced by a model
 * of the chip's memory; the fingers it posts are recorded.
 *
 *   touch_goodix_poll_test SCENARIO
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

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

/* ---- the chip's model: its memory at the touch report, what was read and written ---------------- */

#define REPORT          0x10308         /* where this chip keeps its touch report */

static struct {
	uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	unsigned reads, writes;
	uint32_t last_read_address, last_read_length;
	uint32_t written_address;
	uint8_t written;
} chip;

static bool
model_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t address = (uint32_t)tx[1] << 24 | tx[2] << 16 | tx[3] << 8 | tx[4];

	if (tx[0] == 0xf1) {                    /* a read: eight bytes of header, then the data */
		chip.reads++;
		chip.last_read_address = address;
		chip.last_read_length = length - 8;
		for (uint32_t i = 0; i < length - 8; i++) {
			uint32_t at = address - REPORT + i;

			rx[8 + i] = at < sizeof(chip.report) ? chip.report[at] : 0;
		}
		return true;
	}
	chip.writes++;                          /* (0xf0) a write */
	chip.written_address = address;
	chip.written = tx[5];
	return true;
}

static void
sum_into(uint8_t *block, uint32_t length)       /* the 16-bit sum of the bytes before the last two, there */
{
	uint32_t sum = 0;

	for (uint32_t i = 0; i < length - 2; i++) {
		sum += block[i];
	}
	block[length - 2] = (uint8_t)sum;
	block[length - 1] = (uint8_t)(sum >> 8);
}

/* the chip with a report: STATUS, COUNT fingers (finger i number i, at x 100 * (i + 1), y 200 * (i + 1)) */
static void
report(uint8_t status, unsigned count)
{
	memset(chip.report, 0, sizeof(chip.report));
	chip.report[0] = status;
	chip.report[2] = (uint8_t)count;
	chip.report[4] = 0x5a;                  /* (so that a header is not all zeros) */
	sum_into(chip.report, 8);
	for (unsigned i = 0; i < count && i < GOODIX_MAX_TOUCHES; i++) {
		uint8_t *t = chip.report + 8 + 8 * i;

		t[0] = (uint8_t)(i << 4);
		t[2] = (uint8_t)(100 * (i + 1));
		t[3] = (uint8_t)(100 * (i + 1) >> 8);
		t[4] = (uint8_t)(200 * (i + 1));
		t[5] = (uint8_t)(200 * (i + 1) >> 8);
	}
	if (count > 0 && count <= GOODIX_MAX_TOUCHES) {
		sum_into(chip.report + 8, 8 * count + 2);
	}
	chip.reads = chip.writes = 0;
}

static void
phone(void)
{
	board = "phone-video";
	spi_transfer = model_transfer;
	touch_address = REPORT;
	max_x = 1000;
	max_y = 2000;
}

static bool
was(unsigned n, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	return n < posts && posted[n].type == type && posted[n].finger == finger && posted[n].x == x && posted[n].y == y;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* one finger: one read, the chip told it has been read, the finger down where it is, in 0 to 65535 */
static void
one_finger(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	CHECK(chip.reads == 1 && chip.last_read_address == REPORT && chip.last_read_length == 8 + 16 + 2, "one read of the header "
	    "and two fingers' room: %u, %#x, %u", chip.reads, chip.last_read_address, chip.last_read_length);
	CHECK(chip.writes == 1 && chip.written_address == REPORT && chip.written == 0, "the chip told: %u writes", chip.writes);
	CHECK(posts == 1 && was(0, HAKIOS_TOUCH_DOWN, 0, 100 * 65535 / 1000, 200 * 65535 / 2000), "down: %u posts, %u %u", posts,
	    posted[0].x, posted[0].y);
}

/* two fingers: all in the first read, no second one */
static void
two_fingers(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 2);
	goodix_poll();
	CHECK(chip.reads == 1 && posts == 2, "one read for two: %u reads, %u posts", chip.reads, posts);
}

/* three fingers: the rest read after the header, from where it ends */
static void
three_fingers(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 3);
	goodix_poll();
	CHECK(chip.reads == 2 && chip.last_read_address == REPORT + 8 && chip.last_read_length == 8 * 3 + 2, "then the three: %u "
	    "reads, the last at %#x, %u bytes", chip.reads, chip.last_read_address, chip.last_read_length);
	CHECK(posts == 3 && was(2, HAKIOS_TOUCH_DOWN, 2, 300 * 65535 / 1000, 600 * 65535 / 2000), "three down: %u", posts);
}

/* ten, the most: read; eleven: not believed */
static void
the_most_fingers(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, GOODIX_MAX_TOUCHES);
	goodix_poll();
	CHECK(posts == GOODIX_MAX_TOUCHES, "ten down: %u", posts);
	report(GOODIX_STATUS_TOUCH, GOODIX_MAX_TOUCHES + 1);
	chip.report[2] = GOODIX_MAX_TOUCHES + 1;
	sum_into(chip.report, 8);
	posts = 0;
	seen.bad = 0;
	goodix_poll();
	CHECK(posts == 0 && seen.bad == 1, "eleven: a bad report, none lifted or moved: %u posts, %u bad", posts, seen.bad);
}

/* no report: nothing written, nothing posted */
static void
nothing_new(void)
{
	phone();
	report(0, 0);
	chip.report[0] = 0;
	goodix_poll();
	CHECK(chip.reads == 1 && chip.writes == 0 && posts == 0, "%u reads, %u writes, %u posts", chip.reads, chip.writes, posts);
}

/* a header whose sum is wrong: read and told, but not believed */
static void
bad_header(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	chip.report[6] ^= 1;
	goodix_poll();
	CHECK(chip.writes == 1 && posts == 0 && seen.bad == 1, "%u writes, %u posts, %u bad", chip.writes, posts, seen.bad);
}

/* fingers whose sum is wrong: not believed */
static void
bad_fingers(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	chip.report[8 + 8] ^= 1;
	goodix_poll();
	CHECK(posts == 0 && seen.bad == 1, "%u posts, %u bad", posts, seen.bad);
}

/* a finger at the edge of what the board says: the edge, 65535, not past it */
static void
at_the_edge(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	chip.report[8 + 2] = (uint8_t)1000;
	chip.report[8 + 3] = 1000 >> 8;
	chip.report[8 + 4] = (uint8_t)2000;
	chip.report[8 + 5] = 2000 >> 8;
	sum_into(chip.report + 8, 8 + 2);
	goodix_poll();
	CHECK(was(0, HAKIOS_TOUCH_DOWN, 0, 65535, 65535), "at the edge: %u %u", posted[0].x, posted[0].y);
}

/* the chip asks to be reset: reset, said, and nothing posted */
static void
asks_to_be_reset(void)
{
	phone();
	report(GOODIX_STATUS_REQUEST, 0);
	chip.report[2] = GOODIX_REQUEST_RESET;
	sum_into(chip.report, 8);
	goodix_poll();
	CHECK(logged("the chip asks to be reset") && posts == 0, "%s", log_text);
}

/* a request that is not a reset: nothing */
static void
another_request(void)
{
	phone();
	report(GOODIX_STATUS_REQUEST, 0);
	chip.report[2] = 1;
	sum_into(chip.report, 8);
	goodix_poll();
	CHECK(!logged("asks to be reset") && posts == 0, "%s", log_text);
}

/* the finger lifted: it is not in the next report */
static void
lifted(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	report(GOODIX_STATUS_TOUCH, 0);
	goodix_poll();
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 0, "up: %u posts", posts);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "one_finger", one_finger }, { "two_fingers", two_fingers }, { "three_fingers", three_fingers }, { "the_most_fingers", the_most_fingers },
		{ "nothing_new", nothing_new }, { "bad_header", bad_header }, { "bad_fingers", bad_fingers },
		{ "at_the_edge", at_the_edge }, { "asks_to_be_reset", asks_to_be_reset }, { "another_request", another_request },
		{ "lifted", lifted },
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
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
