/*
 * How the FocalTech FT3658U driver (drivers/board/fts.c) reads the controller over SPI: one read is
 * four header bytes (the register, 0xa0, the length), three dummy bytes, the data and its CRC, and a
 * read whose CRC is wrong or whose status byte says busy is made again, three times at most. The
 * driver is compiled as it is into a host program and its SPI transfer is replaced by a model of the
 * controller; also whether the clock controller's bus clocks count as running.
 *
 *   touch_fts_read_test SCENARIO
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* the clock controller's registers, where the test puts it (GCC_BASE); everything else reads 0 */
#define GCC_BASE        0x100000u
#define GCC_WORDS       (0x60000u / 4)
static uint32_t gcc_reg[GCC_WORDS];
static uint32_t refused_votes;          /* vote bits the controller does not take */
static bool command_sticks;             /* the engine's clock never takes its configuration */
static uint32_t se_clock_offset;        /* (the board's se_clock, for the command register's bit) */
static uint32_t vote_offset = ~0u;      /* (the board's vote register) */

uint32_t
hakios_test_rd(vm_offset_t address)
{
	if (address >= GCC_BASE && address < GCC_BASE + 4 * GCC_WORDS) {
		return gcc_reg[(address - GCC_BASE) / 4];
	}
	return 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	if (address >= GCC_BASE && address < GCC_BASE + 4 * GCC_WORDS) {
		uint32_t offset = (uint32_t)(address - GCC_BASE);

		if (offset == se_clock_offset + 0x04 && !command_sticks) {
			value &= ~1u;                   /* (the configuration taken at once) */
		}
		if (offset == vote_offset) {
			value &= ~refused_votes;
		}
		gcc_reg[offset / 4] = value;
	}
}
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_touch_post(uint32_t type __unused, uint32_t finger __unused, uint32_t x __unused, uint32_t y __unused) { }
void hakios_tl(const char *format __unused, ...) { }
void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/fts.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the controller's model: what it answers a read with ------------------------------------------- */

static struct {
	uint8_t data[FTS_MAX_DATA];             /* what its registers hold */
	unsigned transfers;
	uint32_t length;                        /* of the last transfer */
	uint8_t header[4];                      /* of the last transfer */
	unsigned bad_crc, busy, failed;         /* so many of the next answers are spoilt so */
	bool flip_high_crc;                     /* the CRC's high byte wrong, its low byte right */
	bool by_register;                       /* answers with its registers from the one asked for on, not data */
	uint8_t reg[256];
} chip;

static bool
model_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t n = length - FTS_READ_PREFIX - 2;
	uint16_t crc;

	chip.transfers++;
	chip.length = length;
	memcpy(chip.header, tx, 4);
	if (chip.failed > 0) {
		chip.failed--;
		return false;
	}
	if (chip.by_register) {
		for (uint32_t i = 0; i < n; i++) {
			rx[FTS_READ_PREFIX + i] = chip.reg[(tx[0] + i) & 0xff];
		}
	} else {
		memcpy(rx + FTS_READ_PREFIX, chip.data, n);
	}
	crc = fts_crc(rx + FTS_READ_PREFIX, n);
	/* (one spoilt answer at a time: a wrong CRC first, then a busy status) */
	if (chip.bad_crc > 0) {
		chip.bad_crc--;
		crc ^= 0x0001;
	} else if (chip.busy > 0) {
		chip.busy--;
		rx[3] = 0x80;
	} else if (chip.flip_high_crc) {
		crc ^= 0x0100;
	}
	rx[FTS_READ_PREFIX + n] = (uint8_t)crc;
	rx[FTS_READ_PREFIX + n + 1] = (uint8_t)(crc >> 8);
	return true;
}

static void
controller(void)
{
	for (unsigned i = 0; i < sizeof(chip.data); i++) {
		chip.data[i] = (uint8_t)(i * 37 + 11);
	}
	spi_transfer = model_transfer;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* a read: the header, the length on the wire, the data as the controller has it */
static void
a_read(void)
{
	uint8_t got[FTS_MAX_DATA] = { 0 };

	controller();
	CHECK(fts_read(0x01, got, FTS_MAX_DATA), "the whole report read");
	CHECK(chip.transfers == 1 && chip.length == FTS_READ_PREFIX + FTS_MAX_DATA + 2, "one transfer of %u bytes: %u of %u",
	    FTS_READ_PREFIX + FTS_MAX_DATA + 2, chip.transfers, chip.length);
	CHECK(chip.header[0] == 0x01 && chip.header[1] == 0xa0 && chip.header[2] == 0 && chip.header[3] == FTS_MAX_DATA,
	    "the header: %02x %02x %02x %02x", chip.header[0], chip.header[1], chip.header[2], chip.header[3]);
	CHECK(memcmp(got, chip.data, FTS_MAX_DATA) == 0, "the data as the controller has it");
	CHECK(seen.transfers_failed == 0, "%u failed", seen.transfers_failed);
}

/* no data, or more than a report: not asked */
static void
lengths_refused(void)
{
	uint8_t got[FTS_MAX_DATA + 1];

	controller();
	CHECK(!fts_read(0x01, got, 0) && !fts_read(0x01, got, FTS_MAX_DATA + 1), "refused");
	CHECK(chip.transfers == 0, "and nothing sent: %u transfers", chip.transfers);
	CHECK(fts_read(0xa3, got, 1) && got[0] == chip.data[0], "one byte is a read");
}

/* a wrong CRC, a busy controller or a failed transfer: read again, up to three times */
static void
read_again(void)
{
	uint8_t got[4] = { 0 };

	controller();
	chip.bad_crc = 1;
	chip.busy = 1;
	CHECK(fts_read(0x01, got, sizeof(got)) && chip.transfers == 3, "the third answer is taken: %u transfers", chip.transfers);
	CHECK(memcmp(got, chip.data, sizeof(got)) == 0, "with its data");
	chip.transfers = 0;
	chip.failed = 2;
	CHECK(fts_read(0x01, got, sizeof(got)) && chip.transfers == 3, "after two failed transfers: %u", chip.transfers);
	CHECK(seen.transfers_failed == 0, "no read has failed yet: %u", seen.transfers_failed);
}

/* three bad answers: the read fails, nothing is taken, and it is counted */
static void
read_given_up(void)
{
	uint8_t got[4] = { 0xee, 0xee, 0xee, 0xee };

	controller();
	chip.bad_crc = 3;
	CHECK(!fts_read(0x01, got, sizeof(got)) && chip.transfers == 3, "given up after three: %u", chip.transfers);
	CHECK(got[0] == 0xee && got[3] == 0xee, "nothing of a bad answer taken");
	CHECK(seen.transfers_failed == 1, "counted: %u", seen.transfers_failed);
	chip.transfers = 0;
	chip.busy = 3;
	CHECK(!fts_read(0x01, got, sizeof(got)) && chip.transfers == 3 && seen.transfers_failed == 2, "busy three times: %u",
	    chip.transfers);
}

/* the CRC is two bytes, low first: an answer right in its low byte and wrong in its high one is wrong */
static void
crc_both_bytes(void)
{
	uint8_t got[FTS_MAX_DATA];

	controller();
	chip.flip_high_crc = true;
	CHECK(!fts_read(0x01, got, FTS_MAX_DATA) && chip.transfers == 3, "refused: %u transfers", chip.transfers);
}

/*
 * The engine's clock (clocks_on): with presets (DFSR bit 0) the lowest of them at or above the rate asked
 * for, in whatever order the eight are; without, the crystal. The phone-cmd board's touch { } wiring.
 */
#define CFG(source, divider)    ((source) << 8 | (divider))     /* (mode 0: no M/N) */

static void
clock_controller(bool presets)
{
	board = "phone-cmd";
	gcc = GCC_BASE;
	se_clock_offset = hakios_board()->touch.se_clock;
	vote_offset = hakios_board()->touch.vote_register;
	gcc_reg[(se_clock_offset + 0x18) / 4] = presets;
	/* 100, 19.2, 50, 7.68, 300 (the 300 MHz source undivided), 19.2 again, 0 (a source the engine has not), and
	 * 12.8 MHz last */
	static const uint32_t cfg[8] = { CFG(1, 11), CFG(0, 0), CFG(1, 23), CFG(0, 4), CFG(6, 0), CFG(0, 0), CFG(2, 0), CFG(0, 2) };
	for (unsigned l = 0; l < 8; l++) {
		gcc_reg[(se_clock_offset + 0x20 + 4 * l) / 4] = cfg[l];
	}
}

static void
lowest_preset_above(void)
{
	clock_controller(true);
	CHECK(clocks_on(12000000) && se_source_hz == 12800000 && se_clock_select == 7, "12 MHz: 12.8 MHz, the last preset: %u, %u",
	    se_source_hz, se_clock_select);
}

/* two presets of the same rate: the first */
static void
first_of_two_alike(void)
{
	clock_controller(true);
	CHECK(clocks_on(15000000) && se_source_hz == 19200000 && se_clock_select == 1, "15 MHz: 19.2 MHz, preset 1 not 5: %u, %u",
	    se_source_hz, se_clock_select);
}

static void
preset_of_the_rate_asked(void)
{
	clock_controller(true);
	CHECK(clocks_on(50000000) && se_source_hz == 50000000 && se_clock_select == 2, "50 MHz: preset 2 exactly: %u, %u",
	    se_source_hz, se_clock_select);
}

static void
no_preset_high_enough(void)
{
	clock_controller(true);
	CHECK(!clocks_on(400000000) && se_source_hz == 0, "400 MHz: none: %u", se_source_hz);
}

static void
the_crystal_without_presets(void)
{
	clock_controller(false);
	gcc_reg[(se_clock_offset + 0x08) / 4] = 0x371fu | 0x4000u;      /* (a configuration of before, a bit outside it) */
	CHECK(clocks_on(12000000) && se_source_hz == XO_HZ && se_clock_select == 0, "the crystal: %u", se_source_hz);
	CHECK(gcc_reg[(se_clock_offset + 0x08) / 4] == (0x4000u | 1), "its configuration: source 0, divider 1: %#x",
	    gcc_reg[(se_clock_offset + 0x08) / 4]);
}

static void
configuration_not_taken(void)
{
	clock_controller(false);
	command_sticks = true;
	CHECK(!clocks_on(12000000), "the engine's clock never takes it: refused");
}

static void
votes_refused(void)
{
	clock_controller(true);
	refused_votes = 1u << hakios_board()->touch.vote[4];
	CHECK(!clocks_on(12000000), "the engine's own vote not taken: refused");
}

static void
clocks_stay_off(void)
{
	clock_controller(true);
	gcc_reg[hakios_board()->touch.se_clock / 4] = CLOCK_OFF;
	CHECK(!clocks_on(12000000), "the engine's clock off: refused");
}

/* the clock controller's bus clocks: running when not off, or off but able to run (bit 1, idle) */
static void
bus_clock_ready(void)
{
	CHECK(fts_ahb_clock_ready(0), "on");
	CHECK(fts_ahb_clock_ready(CLOCK_OFF | 2), "off while idle, voted for");
	CHECK(!fts_ahb_clock_ready(CLOCK_OFF), "off");
}

/* ---- the probe: the application's ID, its work mode, its firmware ------------------------------------- */

/* a controller running its firmware, as the phone's comes up: the ID 0x5652 at 0xa3/0x9f, work mode at 0x00 */
static void
running_firmware(uint8_t mode, uint8_t high, uint8_t low)
{
	controller();
	chip.by_register = true;
	chip.reg[0xa3] = high;
	chip.reg[0x9f] = low;
	chip.reg[0x00] = mode;
	chip.reg[0xa6] = 0x12;
}

static void
probed(void)
{
	running_firmware(0x00, 0x56, 0x52);
	CHECK(fts_probe(), "found: %s", log_text);
	CHECK(logged("fts: FT3658U ID 0x5652, firmware 0x12"), "said: %s", log_text);
}

/* in a mode other than the application's work mode (factory, test): not used */
static void
not_in_work_mode(void)
{
	running_firmware(0x40, 0x56, 0x52);
	CHECK(!fts_probe() && logged("fts: chip 0x5652 is not in application work mode (0x40)"), "said: %s", log_text);
	running_firmware(0x10, 0x56, 0x52);
	CHECK(!fts_probe() && logged("not in application work mode (0x10)"), "the mode's lowest bit as well: %s", log_text);
}

/* another ID (the boot loader's, half of it, or none): no firmware, after ten looks */
static void
another_id(void)
{
	running_firmware(0x00, 0x56, 0x53);
	CHECK(!fts_probe() && logged("fts: no application ID 0x5652 (last 0x5653)"), "said: %s", log_text);
	running_firmware(0x00, 0x57, 0x52);
	CHECK(!fts_probe() && logged("(last 0x5752)"), "said: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_read", a_read }, { "probed", probed }, { "not_in_work_mode", not_in_work_mode }, { "another_id", another_id }, { "lengths_refused", lengths_refused }, { "read_again", read_again },
		{ "read_given_up", read_given_up }, { "crc_both_bytes", crc_both_bytes }, { "bus_clock_ready", bus_clock_ready },
		{ "lowest_preset_above", lowest_preset_above }, { "first_of_two_alike", first_of_two_alike }, { "preset_of_the_rate_asked", preset_of_the_rate_asked },
		{ "no_preset_high_enough", no_preset_high_enough }, { "the_crystal_without_presets", the_crystal_without_presets },
		{ "configuration_not_taken", configuration_not_taken }, { "votes_refused", votes_refused },
		{ "clocks_stay_off", clocks_stay_off },
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
