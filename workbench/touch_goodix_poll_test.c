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

/* the FocalTech scenarios' chip follows its reset line: where the driver writes the line's level, and the level */
static vm_offset_t reset_io = ~(vm_offset_t)0;
static int reset_high = 1;

uint32_t hakios_test_rd(vm_offset_t address __unused) { return 0; }
void hakios_test_wr(vm_offset_t address, uint32_t value) { if (address == reset_io) { reset_high = (value >> 1) & 1; } }
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
