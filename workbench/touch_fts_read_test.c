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

uint32_t hakios_test_rd(vm_offset_t address __unused) { return 0; }
void hakios_test_wr(vm_offset_t address __unused, uint32_t value __unused) { }
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
	memcpy(rx + FTS_READ_PREFIX, chip.data, n);
	crc = fts_crc(chip.data, n);
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

/* the clock controller's bus clocks: running when not off, or off but able to run (bit 1, idle) */
static void
bus_clock_ready(void)
{
	CHECK(fts_ahb_clock_ready(0), "on");
	CHECK(fts_ahb_clock_ready(CLOCK_OFF | 2), "off while idle, voted for");
	CHECK(!fts_ahb_clock_ready(CLOCK_OFF), "off");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_read", a_read }, { "lengths_refused", lengths_refused }, { "read_again", read_again },
		{ "read_given_up", read_given_up }, { "crc_both_bytes", crc_both_bytes }, { "bus_clock_ready", bus_clock_ready },
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
