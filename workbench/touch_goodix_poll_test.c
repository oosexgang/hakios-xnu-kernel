/*
 * How the Goodix driver (drivers/board/goodix.c) reads a report: the header first with two fingers'
 * room, the rest only when there are more, the chip told it has been read, the header's and the
 * touches' checksums checked before a finger is believed, and a request of the chip's to be reset
 * honoured; fingers not left down when the chip falls silent; and the chip powered down again when it
 * is not found and at the run's end. The driver is compiled as it is into a host program, its SPI
 * transfer replaced by a model of the chip's memory; the fingers it posts are recorded. For the
 * driver's own thread, the pins, the clock votes and RPMh are modelled too, and the chip answers on
 * the pins driven by hand.
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

/* the FocalTech scenarios' chip follows its reset line: where the driver writes the line's level, and the level */
static vm_offset_t reset_io = ~(vm_offset_t)0;
static int reset_high = 1;

/* the pins, the clock controller's votes and RPMh, once a scenario has the driver map them (after the driver: model_*()) */
static uint32_t model_rd(vm_offset_t address);
static void model_wr(vm_offset_t address, uint32_t value);
static vm_offset_t model_database(vm_offset_t phys);

uint32_t hakios_test_rd(vm_offset_t address) { return model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { if (address == reset_io) { reset_high = (value >> 1) & 1; } model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return model_database(phys); }
void hakios_tl(const char *format __unused, ...) { }
static unsigned rpmh_locks;             /* (taken in a thread's context; at the run's end, where the other cores are stopped, not) */
void hakios_rpmh_lock(void) { rpmh_locks++; }
void hakios_rpmh_unlock(void) { }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/goodix.c"
#undef printf

/* (the run's end's put-away: looked for, so that a driver without one is a scenario that fails, not a test that does not build) */
#pragma weak hakios_goodix_quiesce

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the chip's model: its memory at the touch report, what was read and written ---------------- */

#define REPORT          0x10308         /* where this chip keeps its touch report */
#define COMMAND         0x10180         /* ... and its command buffer, as its description says */

static struct {
	uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	uint8_t version[28], info[GOODIX_MAX_READ];
	uint8_t command[16];                    /* what was last written into the command buffer */
	uint8_t acks[8];                        /* the acknowledgements it answers with, one a look, the last again */
	unsigned acks_n, looks, commands;
	unsigned reads, writes;
	uint32_t last_read_address, last_read_length;
	uint32_t written_address;
	uint8_t written;
} chip;

/* the byte of the chip's memory at an address: its report, its version, its description, its command buffer */
static uint8_t
memory_at(uint32_t at)
{
	if (at >= REPORT && at - REPORT < sizeof(chip.report)) {
		return chip.report[at - REPORT];
	}
	if (at >= GOODIX_FW_VERSION && at - GOODIX_FW_VERSION < sizeof(chip.version)) {
		return chip.version[at - GOODIX_FW_VERSION];
	}
	if (at >= GOODIX_IC_INFO && at - GOODIX_IC_INFO < sizeof(chip.info) && at < COMMAND) {
		return chip.info[at - GOODIX_IC_INFO];
	}
	if (at >= COMMAND && at - COMMAND < 16) {
		return at - COMMAND == 1 ? chip.acks[chip.looks < chip.acks_n ? chip.looks : chip.acks_n - 1] : chip.command[at - COMMAND];
	}
	return 0;
}

static bool
model_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t address = (uint32_t)tx[1] << 24 | tx[2] << 16 | tx[3] << 8 | tx[4];

	if (tx[0] == 0xf1) {                    /* a read: eight bytes of header, then the data */
		chip.reads++;
		chip.last_read_address = address;
		chip.last_read_length = length - 8;
		for (uint32_t i = 0; i < length - 8; i++) {
			rx[8 + i] = memory_at(address + i);
		}
		if (address == COMMAND && chip.acks_n != 0) {
			chip.looks++;
		}
		return true;
	}
	chip.writes++;                          /* (0xf0) a write */
	chip.written_address = address;
	chip.written = tx[5];
	if (address == COMMAND) {
		memcpy(chip.command, tx + 5, 16);
		chip.commands++;
		chip.looks = 0;
	}
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

/*
 * The chip's version (a name, its firmware, a sum) and its description: its length, a version, features and counts,
 * five empty lists, then the "misc" part -- the command buffer's address first, the report's 44 bytes on, the panel's
 * coordinates 138 bytes on -- and a sum. LENGTH: what the description says it is (the misc part whole from 181).
 */
#define LISTS_END       (2 + 16 + 10 + 4 + 5)

static void
described(unsigned length, uint16_t x, uint16_t y)
{
	memset(chip.version, 0, sizeof(chip.version));
	memcpy(chip.version, "GT9916", 6);
	memcpy(chip.version + 10, "01020304", 8);
	sum_into(chip.version, sizeof(chip.version));
	memset(chip.info, 0, sizeof(chip.info));
	chip.info[0] = (uint8_t)length;
	chip.info[1] = (uint8_t)(length >> 8);
	if (length >= LISTS_END + 144) {
		uint8_t *misc = chip.info + LISTS_END;

		misc[0] = (uint8_t)COMMAND;
		misc[1] = (uint8_t)(COMMAND >> 8);
		misc[2] = (uint8_t)(COMMAND >> 16);
		misc[44] = (uint8_t)REPORT;
		misc[45] = (uint8_t)(REPORT >> 8);
		misc[46] = (uint8_t)(REPORT >> 16);
		misc[138] = (uint8_t)x;
		misc[139] = (uint8_t)(x >> 8);
		misc[140] = (uint8_t)y;
		misc[141] = (uint8_t)(y >> 8);
	}
	if (length >= 2 && length <= sizeof(chip.info)) {
		sum_into(chip.info, length);
	}
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

/* the request's low four bits are what it asks for, whatever is above them */
static void
asks_to_be_reset_with_more_bits(void)
{
	phone();
	report(GOODIX_STATUS_REQUEST, 0);
	chip.report[2] = 0x10 | GOODIX_REQUEST_RESET;
	sum_into(chip.report, 8);
	goodix_poll();
	CHECK(logged("the chip asks to be reset") && posts == 0, "%s", log_text);
}

/* the chip's description: where its report and its command buffer are, and the panel's coordinates */
static void
the_chips_description(void)
{
	phone();
	touch_address = command_address = 0;
	described(LISTS_END + 144, 1080, 2400);
	CHECK(goodix_probe(true), "probed: %s", log_text);
	CHECK(touch_address == REPORT && command_address == COMMAND && max_x == 1080 && max_y == 2400, "report %#x, commands %#x, %u x %u",
	    touch_address, command_address, max_x, max_y);
	CHECK(logged("goodix: chip \"GT9916\", firmware \"01020304\"") && logged("coordinates up to 1080 x 2400"), "said: %s", log_text);
}

/* a description without the panel's coordinates, or with one of them 0: the board's are kept */
static void
coordinates_not_described(void)
{
	phone();
	described(LISTS_END + 144, 0, 2400);
	CHECK(goodix_probe(true) && max_x == 1000 && max_y == 2000, "the board's: %u x %u", max_x, max_y);
	described(LISTS_END + 144, 1080, 0);
	CHECK(goodix_probe(true) && max_x == 1000 && max_y == 2000, "the board's: %u x %u", max_x, max_y);
}

/* a description shorter than its fixed part, or as long as a read can be: not believed, and said so */
static void
descriptions_not_believed(void)
{
	phone();
	described(39, 0, 0);
	CHECK(!goodix_probe(true) && logged("goodix: bad chip description (length 39)"), "39 bytes: %s", log_text);
	described(GOODIX_MAX_READ, 0, 0);
	CHECK(!goodix_probe(true) && logged("bad chip description (length 1024)"), "1024 bytes: %s", log_text);
	described(LISTS_END + 49, 0, 0);
	CHECK(!goodix_probe(true) && logged("chip description too short"), "lists, and no room for the report's address: %s", log_text);
}

/* a command: written whole, with its sum; done once the chip acknowledges, waited for while it is busy */
static void
a_command(void)
{
	phone();
	command_address = COMMAND;
	chip.acks[0] = GOODIX_ACK_OK;
	chip.acks_n = 1;
	CHECK(goodix_command(GOODIX_CMD_HIGH_RATE, 1), "done");
	CHECK(chip.commands == 1 && chip.command[2] == 5 && chip.command[3] == GOODIX_CMD_HIGH_RATE && chip.command[4] == 1 &&
	    chip.command[5] == (uint8_t)(5 + GOODIX_CMD_HIGH_RATE + 1) && chip.command[6] == 0, "written as Android writes it");
	chip.acks[0] = chip.acks[1] = GOODIX_ACK_BUSY;
	chip.acks[2] = GOODIX_ACK_OK;
	chip.acks_n = 3;
	chip.commands = 0;
	CHECK(goodix_command(GOODIX_CMD_HIGH_RATE, 1) && chip.commands == 1, "busy twice, then done: written once (%u)", chip.commands);
	chip.acks[0] = GOODIX_ACK_BUSY;
	chip.acks_n = 1;
	chip.commands = 0;
	CHECK(!goodix_command(GOODIX_CMD_HIGH_RATE, 1) && chip.commands == 6, "busy all along: not done, written six times (%u)",
	    chip.commands);
	chip.acks[0] = 0x55;
	chip.commands = 0;
	CHECK(!goodix_command(GOODIX_CMD_HIGH_RATE, 1) && chip.commands == 6, "another answer: written again at once, six times (%u)",
	    chip.commands);
	command_address = 0;
	chip.commands = chip.writes = 0;
	CHECK(!goodix_command(GOODIX_CMD_HIGH_RATE, 1) && chip.writes == 0, "no command buffer described: nothing written anywhere");
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

/* a finger down when the chip asks to be reset: lifted, for the reports after a reset do not say so */
static void
reset_lifts(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	report(GOODIX_STATUS_REQUEST, 0);
	chip.report[2] = GOODIX_REQUEST_RESET;
	sum_into(chip.report, 8);
	goodix_poll();
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 0, "lifted at the reset: %u posts", posts);
}

static bool
transfer_fails(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	(void)tx;
	(void)rx;
	(void)length;
	return false;
}

/* a finger down, then the reads failing one after another: lifted once, after so many, not at the first */
static void
reads_failing_lift(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	spi_transfer = transfer_fails;
	for (unsigned i = 0; i < READS_FAILED_MOST - 1; i++) {
		goodix_poll();
	}
	spi_transfer = model_transfer;                  /* (a read that goes starts the count again) */
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	spi_transfer = transfer_fails;
	for (unsigned i = 0; i < READS_FAILED_MOST - 1; i++) {
		goodix_poll();
	}
	CHECK(posts == 1, "still down after %u failed reads, one gone between", 2 * (READS_FAILED_MOST - 1));
	goodix_poll();
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 0, "lifted: %u posts", posts);
	for (unsigned i = 0; i < 2 * READS_FAILED_MOST; i++) {
		goodix_poll();
	}
	CHECK(posts == 2, "once: %u posts", posts);
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

/* ---- FocalTech's FT3680: no firmware of its own; the boot ROM, or the application the driver sent ---------------- */

static struct {
	bool running;                   /* the application runs (a warm start: the run before this one sent it) */
	unsigned started, pram_writes;  /* start-application commands, PRAM writes */
} ft;

/* an answer of the chip's framing: status 0, then the data and their CRC-16/Kermit, from byte 7 */
static void
ft_answer(uint8_t *rx, const uint8_t *data, uint32_t length)
{
	uint16_t crc = focaltech_crc(data, length);

	memcpy(rx + 7, data, length);
	rx[7 + length] = (uint8_t)crc;
	rx[8 + length] = (uint8_t)(crc >> 8);
}

static bool
ft_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	memset(rx, 0, length);
	if (!reset_high) {
		return true;                    /* in reset: nothing drives the line, and no answer's sum is right */
	}
	if (tx[1] == FOCALTECH_READ_CTRL) {
		static const uint8_t app_high = 0x56, app_low = 0x62, boot[2] = { 0x56, 0x62 };

		if (ft.running && tx[0] == FOCALTECH_CHIP_ID_H) {
			ft_answer(rx, &app_high, 1);
		} else if (ft.running && tx[0] == FOCALTECH_CHIP_ID_L) {
			ft_answer(rx, &app_low, 1);
		} else if (!ft.running && tx[0] == FTS_CMD_READ_BOOT_ID) {
			ft_answer(rx, boot, 2);
		}
		return true;
	}
	if (tx[0] == FTS_ROMBOOT_CMD_START_APP) {
		ft.started++;
	} else if (tx[0] == FTS_ROMBOOT_CMD_WRITE) {
		ft.pram_writes++;
	}
	return true;
}

static void
focaltech_phone(bool running)
{
	board = "phone-video";
	fake_delay_divisor = 100;
	spi_transfer = ft_transfer;
	reset_io = (vm_offset_t)GPIO_IO(PIN_RESET);
	memset(&ft, 0, sizeof(ft));
	ft.running = running;
	gpio_set(PIN_RESET, false);             /* as power_on() leaves it: the chip held in reset */
}

/* a warm start: the application the last run sent still runs, and is found -- once the chip is out of reset */
static void
focaltech_already_running(void)
{
	focaltech_phone(true);
	CHECK(focaltech_probe(true), "found: %s", log_text);
	CHECK(reset_high, "the reset line let go");
	CHECK(ft.started == 0 && ft.pram_writes == 0, "nothing sent again: %u starts, %u writes", ft.started, ft.pram_writes);
}

/* a kernel built without the firmware (the stand-in focaltech_fw.h): said, and the boot ROM is not told to start nothing */
static void
focaltech_without_firmware(void)
{
	focaltech_phone(false);
	CHECK(FOCALTECH_FW_LENGTH == 0, "(the build's stand-in header)");
	CHECK(!focaltech_probe(true), "not started");
	CHECK(strstr(log_text, "no firmware built in") != NULL, "said: %s", log_text);
	CHECK(ft.started == 0 && ft.pram_writes == 0, "no start command, no write into its memory: %u, %u", ft.started, ft.pram_writes);
}

/* ---- a chip that falls silent: what the pins and the engine read then -------------------------------- */

static uint8_t floating;                /* what the data line reads with nothing driving it */

static bool
floating_transfer(const uint8_t *tx __unused, uint8_t *rx, uint32_t length)
{
	if (rx != NULL) {
		memset(rx, floating, length);
	}
	return true;
}

static unsigned
times_logged(const char *text)
{
	unsigned n = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

static uint64_t
ms_since(uint64_t start)
{
	return (mach_absolute_time() - start) / NSEC_PER_MSEC;
}

/*
 * A finger down, then the chip silent while the transfers go: looked at as the thread looks, still down a little
 * while on, lifted once a quarter of a second has gone without a report, once, and said once
 */
static void
fallen_silent(void)
{
	uint64_t start;

	start = mach_absolute_time();
	while (ms_since(start) < 150) {
		goodix_poll();
		usleep(4000);
	}
	CHECK(posts == 1, "not lifted at once: %u posts after 150 ms", posts);
	while (ms_since(start) < 1000 && posts < 2) {
		goodix_poll();
		usleep(4000);
	}
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 0, "lifted within a second: %u posts", posts);
	for (unsigned i = 0; i < 20; i++) {
		goodix_poll();
		usleep(4000);
	}
	CHECK(posts == 2, "once: %u posts", posts);
	CHECK(times_logged("the fingers taken as lifted") == 1, "said once: %s", log_text);
}

/* the bus floating high: every read all ones, a header that is never believed */
static void
silent_bus_high(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	floating = 0xff;
	spi_transfer = floating_transfer;
	fallen_silent();
}

/* the bus floating low: every read all zeros, "nothing new" */
static void
silent_bus_low(void)
{
	phone();
	report(GOODIX_STATUS_TOUCH, 1);
	goodix_poll();
	floating = 0;
	spi_transfer = floating_transfer;
	fallen_silent();
}

static bool
rest_fails(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (tx[0] == 0xf1 && ((uint32_t)tx[1] << 24 | tx[2] << 16 | tx[3] << 8 | tx[4]) == REPORT + 8) {
		return false;
	}
	return model_transfer(tx, rx, length);
}

/* three fingers, then the header read every time and the rest of the report never: lifted all the same */
static void
rest_never_read(void)
{
	uint64_t start;

	phone();
	report(GOODIX_STATUS_TOUCH, 3);
	goodix_poll();
	CHECK(posts == 3, "three down: %u", posts);
	spi_transfer = rest_fails;
	start = mach_absolute_time();
	while (ms_since(start) < 1000 && posts < 6) {
		goodix_poll();
		usleep(4000);
	}
	CHECK(posts == 6 && posted[3].type == HAKIOS_TOUCH_UP && posted[5].type == HAKIOS_TOUCH_UP, "the three lifted: %u posts",
	    posts);
}

/* ---- FocalTech: what a slot's event and the report's count say --------------------------------------- */

/* a report of COUNT fingers with these slots (event, id, x, y), the rest empty */
static void
ft_report(uint8_t *r, unsigned count, const uint32_t (*slots)[4], unsigned n)
{
	memset(r, 0xff, FOCALTECH_TOUCH_REPORT_LEN);
	r[0] = 0;
	r[1] = (uint8_t)count;
	for (unsigned i = 0; i < n; i++) {
		uint8_t *t = r + 2 + 6 * i;

		t[0] = (uint8_t)(slots[i][0] << 6 | (slots[i][2] >> 11 & 0xf));
		t[1] = (uint8_t)(slots[i][2] >> 3);
		t[2] = (uint8_t)(slots[i][1] << 4 | (slots[i][3] >> 11 & 0xf));
		t[3] = (uint8_t)(slots[i][3] >> 3);
		t[4] = (uint8_t)((slots[i][2] & 7) << 5 | (slots[i][3] & 7) << 2);
		t[5] = 0;
	}
}

/* event 3, "no event": not a finger down, and one that was down is up */
static void
focaltech_no_event(void)
{
	static const uint32_t down[1][4] = { { 0, 2, 1000, 2000 } }, none[1][4] = { { 3, 2, 1000, 2000 } },
	    other[1][4] = { { 3, 4, 3000, 4000 } };
	uint8_t r[FOCALTECH_TOUCH_REPORT_LEN];

	focaltech_max_x = 10000;
	focaltech_max_y = 20000;
	ft_report(r, 1, other, 1);
	focaltech_report(r);
	CHECK(posts == 0, "no event in a slot: no finger down (%u posts, type %u finger %u)", posts, posted[0].type, posted[0].finger);
	ft_report(r, 1, down, 1);
	focaltech_report(r);
	CHECK(posts == 1 && was(0, HAKIOS_TOUCH_DOWN, 2, 1000 * 65535 / 9999, 2000 * 65535 / 19999), "down: %u posts", posts);
	ft_report(r, 1, none, 1);
	focaltech_report(r);
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 2, "then no event for it: up (%u posts)", posts);
}

/* a report that says no finger and has one in contact: not believed, the finger left as it was; one that is empty lifts it */
static void
focaltech_count_zero(void)
{
	static const uint32_t down[1][4] = { { 0, 1, 1000, 2000 } }, moved[1][4] = { { 2, 1, 3000, 4000 } };
	uint8_t r[FOCALTECH_TOUCH_REPORT_LEN];

	focaltech_max_x = 10000;
	focaltech_max_y = 20000;
	ft_report(r, 1, down, 1);
	focaltech_report(r);
	CHECK(posts == 1, "down: %u", posts);
	ft_report(r, 0, moved, 1);
	focaltech_report(r);
	CHECK(posts == 1 && focaltech_contact[1].down, "count 0 with a finger in contact: nothing moved or lifted (%u posts)", posts);
	ft_report(r, 0, NULL, 0);
	focaltech_report(r);
	CHECK(posts == 2 && was(1, HAKIOS_TOUCH_UP, 1, 1000 * 65535 / 9999, 2000 * 65535 / 19999), "an empty report: up where it "
	    "was (%u posts)", posts);
}

/* the FT3680's report, answered whole and with its CRC to every read of it */
static uint8_t ft_frame[FOCALTECH_TOUCH_REPORT_LEN];

static bool
ft_report_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	memset(rx, 0, length);
	if (tx[1] == FOCALTECH_READ_CTRL && tx[0] == FOCALTECH_TOUCH_DATA && length == 7 + sizeof(ft_frame) + 2) {
		ft_answer(rx, ft_frame, sizeof(ft_frame));
	}
	return true;
}

/*
 * A FocalTech finger down, held, and then the chip silent, its reads failing, looked at every 80 ms as the thread looks
 * with the interrupt line high: lifted after a quarter of a second, as a Goodix finger is -- not after 32 failed reads
 */
static void
focaltech_fallen_silent(void)
{
	static const uint32_t down[1][4] = { { 0, 1, 1000, 2000 } };
	uint64_t start, took;

	board = "phone-video";
	touch_chip = TOUCH_FOCALTECH;
	focaltech_max_x = 10000;
	focaltech_max_y = 20000;
	ft_report(ft_frame, 1, down, 1);
	spi_transfer = ft_report_transfer;
	chip_poll();
	CHECK(posts == 1 && posted[0].type == HAKIOS_TOUCH_DOWN && posted[0].finger == 1, "down: %u posts", posts);
	start = mach_absolute_time();
	while (ms_since(start) < 400) {
		chip_poll();
		usleep(80 * 1000);
	}
	CHECK(posts == 1, "held, its reports read every 80 ms: still down (%u posts)", posts);
	spi_transfer = transfer_fails;
	start = mach_absolute_time();
	while (ms_since(start) < 1500 && posts < 2) {
		chip_poll();
		usleep(80 * 1000);
	}
	took = ms_since(start);
	CHECK(posts == 2 && posted[1].type == HAKIOS_TOUCH_UP && posted[1].finger == 1 && took >= 200 && took < 600,
	    "lifted after a quarter of a second: %u posts after %llu ms", posts, (unsigned long long)took);
}

/* ---- the driver's thread: the pins, the clock votes and RPMh modelled; the chip answers on the pins driven by hand ---- */

#define LDO             0x4a000         /* where the command database says the I/O supply's regulator is */

static struct {
	uint32_t cfg[256], io[256];
	uint32_t int_level;                     /* what the interrupt line reads (0: a report waits) */
	bool int_pulled_up;                     /* the interrupt line's pull-up was switched on, ever ... */
	bool int_pulled_unpowered;              /* ... while the chip had no I/O supply */
	bool present;                           /* a chip on the bus */
	uint8_t tx[8 + GOODIX_MAX_READ];        /* the transfer in progress, as the chip has clocked it in */
	unsigned bits;
} pin;

static struct {
	bool busy, deaf;                        /* the slot set never idle; RPMh never says done */
	uint32_t control, irq_status, enabled, address[16], data[16];
	unsigned triggers;
	bool supply_on, withdrawn;
	bool cs_high_at_off;                    /* chip select driven high when the supply was withdrawn */
} rpmh;

static uint32_t gcc_vote;

static union {
	struct cmd_db_header header;
	uint8_t bytes[0x20000];
} database;

static vm_offset_t
model_database(vm_offset_t phys)
{
	const struct hakios_board *b = hakios_board();

	return b != NULL && phys == b->rpmh.cmd_db ? (vm_offset_t)&database : phys;
}

static bool
driven_high(unsigned n)
{
	return (pin.cfg[n] & CFG_OUTPUT) && (pin.io[n] & 2);
}

static bool
chip_on(void)
{
	return pin.present && rpmh.supply_on && driven_high(PIN_AVDD) && driven_high(PIN_RESET);
}

/* the bit the chip puts on its data-out pin for the bit just clocked */
static uint32_t
chip_out(void)
{
	unsigned at = pin.bits - 1, byte = at / 8;

	if (!chip_on() || driven_high(PIN_CS) || pin.bits == 0 || pin.tx[0] != 0xf1 || byte < 8) {
		return 0;
	}
	return memory_at(((uint32_t)pin.tx[1] << 24 | pin.tx[2] << 16 | pin.tx[3] << 8 | pin.tx[4]) + byte - 8) >> (7 - at % 8) & 1;
}

static uint32_t
model_rd(vm_offset_t address)
{
	const struct hakios_board *b = hakios_board();

	if (tlmm != 0 && address >= tlmm && address < tlmm + TLMM_SIZE) {
		unsigned n = (unsigned)((address - tlmm) / 0x1000);

		if ((address - tlmm) % 0x1000 == 0) {
			return pin.cfg[n];
		}
		return n == PIN_MISO ? chip_out() : n == PIN_INT ? pin.int_level : (pin.io[n] >> 1 & 1) | (pin.io[n] & 2);
	}
	if (gcc != 0 && address == GCC_VOTE) {
		return gcc_vote;
	}
	if (b != NULL && address == b->rpmh.rsc) {
		return 0x00020000;              /* (a version 2 sequencer) */
	}
	if (b != NULL && address >= b->rpmh.rsc + b->rpmh.tcs_offset && address < b->rpmh.rsc + b->rpmh.tcs_offset + 0x2a0) {
		uint32_t reg = (uint32_t)(address - b->rpmh.rsc - b->rpmh.tcs_offset);

		return reg == 0x18 ? !rpmh.busy : reg == 0x04 ? rpmh.irq_status : reg == 0x14 ? rpmh.control : 0;
	}
	return 0;
}

static void
model_wr(vm_offset_t address, uint32_t value)
{
	const struct hakios_board *b = hakios_board();

	if (tlmm != 0 && address >= tlmm && address < tlmm + TLMM_SIZE) {
		unsigned n = (unsigned)((address - tlmm) / 0x1000);

		if ((address - tlmm) % 0x1000 == 0) {
			pin.cfg[n] = value;
			if (n == PIN_INT && (value & 3) == PULL_UP) {
				pin.int_pulled_up = true;
				pin.int_pulled_unpowered |= !rpmh.supply_on;
			}
			return;
		}
		if (n == PIN_CS && (pin.io[n] & 2) && !(value & 2)) {
			pin.bits = 0;                           /* selected: a transfer begins */
			memset(pin.tx, 0, sizeof(pin.tx));
		} else if (n == PIN_CS && !(pin.io[n] & 2) && (value & 2) && pin.bits >= 8 * 8 && chip_on()) {
			uint8_t rx[sizeof(pin.tx)];             /* deselected: what the chip makes of it */

			model_transfer(pin.tx, rx, pin.bits / 8);
		} else if (n == PIN_CLK && !(pin.io[n] & 2) && (value & 2) && !(pin.io[PIN_CS] & 2) && pin.bits < 8 * sizeof(pin.tx)) {
			pin.tx[pin.bits / 8] |= (uint8_t)((pin.io[PIN_MOSI] >> 1 & 1) << (7 - pin.bits % 8));
			pin.bits++;
		}
		pin.io[n] = value;
		return;
	}
	if (gcc != 0 && address == GCC_VOTE) {
		gcc_vote = value;
		return;
	}
	if (b != NULL && address >= b->rpmh.rsc + b->rpmh.tcs_offset && address < b->rpmh.rsc + b->rpmh.tcs_offset + 0x2a0) {
		uint32_t reg = (uint32_t)(address - b->rpmh.rsc - b->rpmh.tcs_offset);

		if (reg == 0x14) {
			if (!(rpmh.control & 1u << 24) && (value & 1u << 24) && (value & 1u << 16)) {
				rpmh.triggers++;
				for (unsigned c = 0; c < 16; c++) {
					if ((rpmh.enabled & 1u << c) && rpmh.address[c] == LDO + 4) {
						rpmh.supply_on = rpmh.data[c] != 0;
						if (!rpmh.supply_on) {
							rpmh.withdrawn = true;
							rpmh.cs_high_at_off = driven_high(PIN_CS);
						}
					}
				}
				if (!rpmh.deaf) {
					rpmh.irq_status |= 1;
				}
			}
			rpmh.control = value;
		} else if (reg == 0x08) {
			rpmh.irq_status &= ~value;
		} else if (reg == 0x1c) {
			rpmh.enabled = value;
		} else if (reg >= 0x30 && (reg - 0x30) / 20 < 16) {
			unsigned c = (reg - 0x30) / 20;

			if ((reg - 0x30) % 20 == 4) {
				rpmh.address[c] = value;
			} else if ((reg - 0x30) % 20 == 8) {
				rpmh.data[c] = value;
			}
		}
	}
}

/* the phone with its pins at rest, the M AHB clock's vote already taken (by the firmware), and RPMh's database */
static void
wired_phone(bool present)
{
	struct cmd_db_entry *entry = (struct cmd_db_entry *)database.header.data;

	phone();
	fake_delay_divisor = 100;
	boot_args = "touchscreen=1";
	memset(&pin, 0, sizeof(pin));
	for (unsigned n = 0; n < 256; n++) {
		pin.io[n] = 2;                          /* (as the firmware leaves them: high, chip select too) */
	}
	pin.int_level = 0;
	pin.present = present;
	memset(&rpmh, 0, sizeof(rpmh));
	gcc_vote = 1u << 20;
	memset(&database, 0, sizeof(database));
	database.header.magic[0] = 0xdb;
	database.header.magic[1] = 0x30;
	database.header.magic[2] = 0x03;
	database.header.magic[3] = 0x0c;
	database.header.slave[0].id = CMD_DB_VRM;
	database.header.slave[0].data_offset = 0x100;
	database.header.slave[0].count = 1;
	memcpy(entry->id, "ldoc2", 5);
	entry->address = LDO;
	report(0, 0);
	described(LISTS_END + 144, 1080, 2400);
}

/* the driver's own mappings, for a scenario that calls one step of its bring-up itself */
static void
mapped(void)
{
	tlmm = TLMM_PHYS;
	gcc = GCC_PHYS;
	rsc = RSC_PHYS;
	cmd_db = (vm_offset_t)&database;
}

static bool
waited_for(const char *text, unsigned ms)
{
	for (unsigned waited = 0; !logged(text) && waited < ms; waited += 5) {
		usleep(5000);
	}
	return logged(text);
}

/* RPMh does not take the I/O supply's request: the interrupt line is not pulled up into a chip without a supply */
static void
int_pull_up_waits_for_supply(void)
{
	wired_phone(true);
	mapped();
	rpmh.busy = true;
	CHECK(!power_on(), "not powered");
	CHECK(!pin.int_pulled_up, "the interrupt line pulled up into a chip without its supply (configuration %#x)", pin.cfg[PIN_INT]);
	rpmh.busy = false;
	CHECK(power_on(), "powered: %s", log_text);
	CHECK(pin.int_pulled_up && !pin.int_pulled_unpowered, "pulled up only once the supply is on: %u, %u", pin.int_pulled_up,
	    pin.int_pulled_unpowered);
}

/* the bus is the pins' as the run found them, not the driver's, once nothing more of it is driven */
static void
let_go(const char *when)
{
	static const char *const names[4] = { "chip select", "the clock", "data out", "data in" };
	const unsigned bus[4] = { PIN_CS, PIN_CLK, PIN_MOSI, PIN_MISO };

	for (unsigned i = 0; i < 4; i++) {
		CHECK(!(pin.cfg[bus[i]] & CFG_OUTPUT) && (pin.cfg[bus[i]] & 3) == PULL_NONE && (pin.cfg[bus[i]] >> 2 & 0xf) == 0,
		    "%s: %s is not let go (configuration %#x)", when, names[i], pin.cfg[bus[i]]);
	}
}

static void
powered_down(const char *when)
{
	CHECK((pin.cfg[PIN_RESET] & CFG_OUTPUT) && !(pin.io[PIN_RESET] & 2), "%s: reset held low (%#x, %#x)", when, pin.cfg[PIN_RESET],
	    pin.io[PIN_RESET]);
	CHECK((pin.cfg[PIN_AVDD] & CFG_OUTPUT) && !(pin.io[PIN_AVDD] & 2), "%s: the analog supply off (%#x, %#x)", when,
	    pin.cfg[PIN_AVDD], pin.io[PIN_AVDD]);
	CHECK(rpmh.withdrawn && !rpmh.supply_on, "%s: the I/O supply's request withdrawn (%u triggers)", when, rpmh.triggers);
	CHECK(rpmh.cs_high_at_off, "%s: chip select raised while the chip still had its supply", when);
	CHECK((pin.cfg[PIN_INT] & 3) == PULL_NONE, "%s: the interrupt line no longer pulled up (%#x)", when, pin.cfg[PIN_INT]);
	CHECK(gcc_vote == 1u << 20, "%s: the votes this driver took given back, the firmware's kept: %#x", when, gcc_vote);
	let_go(when);
}

/* no chip answers: what was switched on for it is switched off again, and the pins driven by hand let go */
static void
not_found_powered_down(void)
{
	wired_phone(false);
	hakios_goodix_init();
	CHECK(waited_for("no goodix touch screen", 5000), "given up: %s", log_text);
	usleep(20 * 1000);
	CHECK(rpmh.triggers >= 1, "(the I/O supply was asked for: %u)", rpmh.triggers);
	powered_down("not found");
}

/* the run's end: the thread stops reading, and the chip is powered down */
static void
run_end_puts_it_away(void)
{
	void (*volatile put_away)(void) = hakios_goodix_quiesce;       /* (NULL where the driver has none) */
	unsigned reads, locks;

	wired_phone(true);
	hakios_goodix_init();
	CHECK(waited_for("touch screen ready", 5000), "found: %s", log_text);
	usleep(20 * 1000);
	CHECK(chip.reads > 0, "the thread reads the chip's report (%u reads)", chip.reads);
	CHECK(put_away != NULL, "the driver has nothing for the run's end (no hakios_goodix_quiesce())");
	if (put_away == NULL) {
		reads = chip.reads;
		usleep(50 * 1000);
		CHECK(chip.reads == reads, "and its thread reads on: %u reads, then %u", reads, chip.reads);
		return;
	}
	locks = rpmh_locks;
	put_away();
	reads = chip.reads;
	usleep(50 * 1000);
	CHECK(chip.reads == reads, "no read after the run's end: %u, then %u", reads, chip.reads);
	CHECK(rpmh_locks == locks, "RPMh's lock not taken at the run's end (%u times)", rpmh_locks - locks);
	powered_down("at the run's end");
	CHECK(logged("goodix: at the run's end"), "said: %s", log_text);
}

/* the run's end in the middle of a transfer on the pins driven by hand: chip select raised before the supply goes */
static void
run_end_mid_transfer(void)
{
	void (*volatile put_away)(void) = hakios_goodix_quiesce;

	wired_phone(true);
	mapped();
	CHECK(power_on(), "powered: %s", log_text);
	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	gpio_set(PIN_CS, false);                /* (selected, and the thread stopped there) */
	CHECK(put_away != NULL, "the driver has nothing for the run's end (no hakios_goodix_quiesce())");
	if (put_away != NULL) {
		put_away();
		powered_down("at the run's end, a transfer cut off");
	}
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "one_finger", one_finger }, { "two_fingers", two_fingers }, { "three_fingers", three_fingers }, { "the_most_fingers", the_most_fingers },
		{ "nothing_new", nothing_new }, { "bad_header", bad_header }, { "bad_fingers", bad_fingers },
		{ "at_the_edge", at_the_edge }, { "asks_to_be_reset", asks_to_be_reset }, { "another_request", another_request },
		{ "lifted", lifted }, { "reset_lifts", reset_lifts }, { "reads_failing_lift", reads_failing_lift }, { "asks_to_be_reset_with_more_bits", asks_to_be_reset_with_more_bits },
		{ "the_chips_description", the_chips_description }, { "coordinates_not_described", coordinates_not_described },
		{ "descriptions_not_believed", descriptions_not_believed }, { "a_command", a_command },
		{ "focaltech_already_running", focaltech_already_running }, { "focaltech_without_firmware", focaltech_without_firmware },
		{ "silent_bus_high", silent_bus_high }, { "silent_bus_low", silent_bus_low }, { "rest_never_read", rest_never_read },
		{ "focaltech_no_event", focaltech_no_event }, { "focaltech_count_zero", focaltech_count_zero }, { "focaltech_fallen_silent", focaltech_fallen_silent },
		{ "int_pull_up_waits_for_supply", int_pull_up_waits_for_supply }, { "not_found_powered_down", not_found_powered_down },
		{ "run_end_puts_it_away", run_end_puts_it_away }, { "run_end_mid_transfer", run_end_mid_transfer },
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
