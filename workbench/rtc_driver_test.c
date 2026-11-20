/*
 * The PMIC clock's driver itself (drivers/board/rtc.c), run as a host program: the kernel's interfaces are
 * workbench/fakekernel, the registers a model of the SoC's PMIC arbiter (spmi_model.h) with the clock's block
 * behind it. Where the arbiter is and which block the clock is comes from the board the driver reads, so a wrong
 * address in the driver is a read of nothing here.
 *
 *   rtc_driver_test SCENARIO      one scenario a process: the driver keeps what it read in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

#include "spmi_model.h"

static struct spmi_peripheral clock_model, other_model;

uint32_t hakios_test_rd(vm_offset_t address) { return spmi_model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the one lock of the bus is the buttons' driver's (bsd/dev/board/buttons.c): here, that it is taken and given back */
static int bus_held;
void hakios_spmi_lock(void) { bus_held++; }
void hakios_spmi_unlock(void) { bus_held--; }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/rtc.c"

static int failures;
/* (to stderr: this driver also prints every line of its log, which main() sends nowhere) */
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* hw.rtc_seconds as a program reads it: the handler's answer (0, or an errno), the value in *seconds */
static int
rtc_seconds(uint64_t *seconds)
{
	struct sysctl_req req = { NULL, 0, seconds, sizeof(*seconds), 0 };

	*seconds = 0;
	return sysctl_rtc_seconds(NULL, NULL, 0, &req);
}

static void
set_count(uint32_t seconds)
{
	for (unsigned i = 0; i < 4; i++) {
		clock_model.reg[0x48 + i] = (uint8_t)(seconds >> (8 * i));
	}
}

/* a phone: the arbiter where its SoC file has it, the clock's block where its board file says, running */
static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	b = hakios_board();
	if (b == NULL) {
		return;
	}
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	clock_model.id = b->rtc.peripheral;
	other_model.id = 0x062;                 /* the clock's alarm, a block further on, in the table first */
	other_model.reg[0x05] = 0x03;
	spmi_channel(2, &other_model, spmi.ee);
	spmi_channel(4, &clock_model, 3);       /* a channel of another system's first, as real tables have */
	spmi_channel(6, &clock_model, spmi.ee);
	clock_model.reg[0x04] = 0x0b;
	clock_model.reg[0x05] = 0x01;           /* RTC_RW */
	clock_model.reg[0x46] = 0x80;           /* running */
	set_count(1234567890);
}

static void
no_write(void)
{
	CHECK(spmi.writes == 0 && spmi.denied == 0 && clock_model.writes == 0, "the driver wrote to the PMIC (%u writes, %u refused)",
	    spmi.writes, spmi.denied);
	CHECK(spmi.elsewhere == 0, "the driver read or wrote where nothing of the arbiter is (%u)", spmi.elsewhere);
	CHECK(bus_held == 0, "the bus given back as often as it was taken (%d)", bus_held);
}

static void
nothing_read(void)
{
	uint64_t seconds;

	CHECK(spmi.transfers == 0, "nothing asked of the bus (%u transfers)", spmi.transfers);
	CHECK(rtc_seconds(&seconds) == ENOENT, "hw.rtc_seconds not answered, as without the driver");
	no_write();
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the count read where the board says, through this system's channel, and given as hw.rtc_seconds */
static void
reads_the_count(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	hakios_rtc_init();
	CHECK(rtc_seconds(&seconds) == 0 && seconds - 1234567890 <= 1, "hw.rtc_seconds is the count: %llu", (unsigned long long)seconds);
	CHECK(logged("rtc: the PMK8350's RTC (slave 0, registers 0x6100, channel 6, type 0xb) reads 1234567890 s"), "said: %s", log_text);
	CHECK(!logged("carried"), "no carry, no second read");
	no_write();
}

/* the count goes on from the read: a program that asks later is not given the boot's second */
static void
counts_on(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	hakios_rtc_init();
	usleep(1200 * 1000);
	CHECK(rtc_seconds(&seconds) == 0 && seconds >= 1234567891 && seconds <= 1234567892, "a second later: %llu",
	    (unsigned long long)seconds);
	no_write();
}

/*
 * The low byte carries while the four are read: the first read has the old low byte 0xff with the high bytes already
 * past the carry (0x000101ff, 256 s ahead). The low byte read again is lower: the four are read anew.
 */
static unsigned count_reads;

static void
ticking(struct spmi_peripheral *p, uint8_t reg)
{
	if (reg != 0x48) {
		return;
	}
	count_reads++;
	/* the first read's low byte is from before the tick, its others from after; every read after it is after */
	p->reg[0x48] = count_reads == 1 ? 0xff : 0x00;
	p->reg[0x49] = 0x01;
	p->reg[0x4a] = 0x01;
	p->reg[0x4b] = 0x00;
}

static void
a_carry(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	clock_model.looked_at = ticking;
	hakios_rtc_init();
	CHECK(rtc_seconds(&seconds) == 0 && seconds - 0x00010100 <= 1, "the count after the carry, not 256 s ahead: %#llx",
	    (unsigned long long)seconds);
	CHECK(count_reads == 3 && logged("read again: the low byte carried"), "read again, and said (%u reads)", count_reads);
	no_write();
}

/* the same low byte twice: no carry, the count as first read, two reads of it */
static void
steady(struct spmi_peripheral *p, uint8_t reg)
{
	(void)p;
	count_reads += reg == 0x48;
}

static void
no_carry(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	set_count(0x5a5a5aff);
	clock_model.looked_at = steady;
	hakios_rtc_init();
	CHECK(rtc_seconds(&seconds) == 0 && seconds - 0x5a5a5aff <= 1, "the count: %#llx", (unsigned long long)seconds);
	CHECK(count_reads == 2, "the four and the low byte once more (%u reads)", count_reads);
	no_write();
}

/* another phone: the arbiter elsewhere and this system its third, the clock on the third PMIC: all of it data */
static void
another_wiring(void)
{
	uint64_t seconds;

	phone("phone-keys");
	set_count(7);
	hakios_rtc_init();
	CHECK(rtc_seconds(&seconds) == 0 && seconds <= 8, "read: %llu", (unsigned long long)seconds);
	CHECK(logged("rtc: the PMX200's RTC (slave 2, registers 0x6000, channel 6"), "where the board says: %s", log_text);
	no_write();
}

/* a phone whose arbiter is of version 7, the clock on a PMIC of the arbiter's second bus, this system the
 * arbiter's second. The first bus has a clock at the same address, through channels of the same numbers,
 * that counts from another time: not the one read. */
static void
second_bus(void)
{
	static struct spmi_peripheral namesake;
	uint64_t seconds;

	phone("phone-buses");
	spmi_version_7(hakios_board()->spmi.observer, 1, 24, 40);
	namesake = clock_model;                 /* (at 1234567890) */
	namesake.bus = 0;
	spmi_channel(4, &namesake, 3);
	spmi_channel(6, &namesake, spmi.ee);
	set_count(7);
	hakios_rtc_init();
	CHECK(rtc_seconds(&seconds) == 0 && seconds <= 8, "read: %llu", (unsigned long long)seconds);
	CHECK(logged("rtc: the PMY300's RTC (slave 1, registers 0x6100, channel 6"), "where the board says: %s", log_text);
	CHECK(spmi.past_the_table == 0, "%u reads of the first bus's part of the tables", spmi.past_the_table);
	no_write();
}

/* the emulator's board names no clock: nothing read, and said */
static void
board_without_a_clock(void)
{
	phone("qemu-virt");
	hakios_rtc_init();
	CHECK(logged("names no clock of a PMIC (rtc { })"), "said: %s", log_text);
	nothing_read();
}

static void
unknown_board(void)
{
	board = "a-phone-nobody-described";
	hakios_rtc_init();
	CHECK(logged("names no clock of a PMIC"), "said: %s", log_text);
	nothing_read();
}

/* rtc=0: not a read, not a word */
static void
off_when_told(void)
{
	phone("phone-cmd");
	boot_args = "rtc=0";
	hakios_rtc_init();
	CHECK(log_text[0] == 0, "nothing said: %s", log_text);
	nothing_read();
}

/* a counter that does not run: starting it would be a write, so it is left, and nothing is given */
static void
stopped(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	clock_model.reg[0x46] = 0x00;
	hakios_rtc_init();
	CHECK(logged("the PMK8350's RTC is stopped (control 0)"), "said: %s", log_text);
	CHECK(rtc_seconds(&seconds) == ENOENT, "no count given");
	no_write();
}

/* the board names a block that is not the clock's (a GPIO, its enable bit at the same place): not taken for seconds */
static void
not_the_clock(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	clock_model.reg[0x04] = 0x10;
	clock_model.reg[0x05] = 0x10;
	hakios_rtc_init();
	CHECK(logged("is of type 0x10, subtype 0x10: not the RTC's block"), "said: %s", log_text);
	CHECK(rtc_seconds(&seconds) == ENOENT, "no count given");
	no_write();
}

/* nobody at the block's address: said, nothing given */
static void
nobody_there(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	clock_model.silent = true;
	hakios_rtc_init();
	CHECK(logged("the transfer failed"), "said: %s", log_text);
	CHECK(rtc_seconds(&seconds) == ENOENT, "no count given");
	no_write();
}

/* the arbiter has no channel for the block: said, and nothing else asked of the bus */
static void
no_channel(void)
{
	phone("phone-cmd");
	spmi.map[4] = spmi.map[6] = 0;
	hakios_rtc_init();
	CHECK(logged("no channel of the arbiter is the PMK8350's RTC (slave 0, registers 0x6100)"), "said: %s", log_text);
	nothing_read();
}

/* a read the arbiter drops: said, and no count made of the bytes that were there before */
static void
dropped(void)
{
	uint64_t seconds;

	phone("phone-cmd");
	spmi.drop_reads = 3;
	hakios_rtc_init();
	CHECK(logged("the transfer was dropped"), "said: %s", log_text);
	CHECK(rtc_seconds(&seconds) == ENOENT, "no count given");
	no_write();
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "reads_the_count", reads_the_count }, { "counts_on", counts_on }, { "a_carry", a_carry }, { "no_carry", no_carry },
		{ "another_wiring", another_wiring }, { "second_bus", second_bus }, { "board_without_a_clock", board_without_a_clock },
		{ "unknown_board", unknown_board }, { "off_when_told", off_when_told }, { "stopped", stopped },
		{ "not_the_clock", not_the_clock }, { "nobody_there", nobody_there }, { "no_channel", no_channel },
		{ "dropped", dropped },
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
