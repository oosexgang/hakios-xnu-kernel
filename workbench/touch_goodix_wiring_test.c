/*
 * Where the Goodix and FocalTech driver (drivers/board/goodix.c) takes its wiring from. The
 * driver is compiled as it is into a host program; nothing of the controller is modelled. What is
 * tested: every address, clock, vote and pin the driver uses is the board's (touch { } of its file); for
 * the phone the driver was written on they are the numbers that used to be in the code; for a board
 * wired otherwise they are that board's; and without touch { } the driver touches nothing.
 *
 *   touch_goodix_wiring_test SCENARIO
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

static unsigned mappings, accesses, writes;
uint32_t hakios_test_rd(vm_offset_t address __unused) { accesses++; return 0; }
void hakios_test_wr(vm_offset_t address __unused, uint32_t value __unused) { accesses++; writes++; }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
void hakios_touch_post(uint32_t type __unused, uint32_t finger __unused, uint32_t x __unused, uint32_t y __unused) { }
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

/* everything the driver's names for its wiring come to, in one order: the engine, three clocks, five votes, six pins */
static void
wiring(uint32_t *w)
{
	gcc = 0;                                /* (the clocks as offsets in the clock controller) */
	w[0] = (uint32_t)SE_PHYS;
	w[1] = (uint32_t)GCC_QUP_M_AHB_CBCR;
	w[2] = (uint32_t)GCC_QUP_S_AHB_CBCR;
	w[3] = (uint32_t)GCC_SE_CBCR;
	w[4] = VOTE_QUP_CORE_2X;
	w[5] = VOTE_QUP_CORE;
	w[6] = VOTE_QUP_M_AHB;
	w[7] = VOTE_QUP_S_AHB;
	w[8] = VOTE_SE;
	w[9] = PIN_MISO;
	w[10] = PIN_MOSI;
	w[11] = PIN_CLK;
	w[12] = PIN_CS;
	w[13] = PIN_INT;
	w[14] = PIN_RESET;
}

static void
same(const uint32_t *is, const uint32_t *was, const char *whose)
{
	static const char *const names[15] = { "the serial engine", "the first bus clock", "the second bus clock", "the engine's clock",
		"the vote for the core clock twice", "the vote for the core clock", "the vote for the first bus clock",
		"the vote for the second bus clock", "the vote for the engine", "the first data pin", "the second data pin", "the clock's pin",
		"the chip select's pin", "the interrupt's pin", "the reset's pin" };

	for (unsigned i = 0; i < 15; i++) {
		uint32_t expected = i >= 4 && i <= 8 ? 1u << was[i] : was[i];

		CHECK(is[i] == expected, "%s: %s is %#x, not %#x", whose, names[i], is[i], expected);
	}
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* on the phone the driver was written on: the numbers that were in its code */
static void
as_it_was(void)
{
	static const uint32_t was[15] = { 0x00a94000, 0x18004, 0x18008, 0x185fc, 18, 19, 20, 21, 27, 52, 53, 54, 55, 81, 105 };
	uint32_t is[15];

	board = "phone-video";
	wiring(is);
	same(is, was, "phone-video");
	/* the engine's rate generator follows its clock at distances that are the same for every engine */
	CHECK(GCC_SE_CMD == was[3] + 0x04 && GCC_SE_CFG == was[3] + 0x08 && GCC_SE_DFSR == was[3] + 0x18 &&
	    GCC_SE_PERF_CFG(2) == was[3] + 0x28 && GCC_SE_PERF_M(1) == was[3] + 0x64 && GCC_SE_PERF_N(0) == was[3] + 0xa0,
	    "the rate generator's registers");
	CHECK((uint32_t)GCC_VOTE == 0x52008, "the register the votes go into: %#x", (uint32_t)GCC_VOTE);
	CHECK(mappings == 0 && accesses == 0, "(looking at the names touches nothing)");
}

/* on a board wired otherwise: that board's, every one of them */
static void
another_wiring(void)
{
	static const uint32_t its[15] = { 0x00b40000, 0x29004, 0x29008, 0x2939c, 3, 4, 1, 2, 30, 140, 141, 142, 143, 7, 201 };
	uint32_t is[15];

	board = "phone-keys";
	wiring(is);
	same(is, its, "phone-keys");
	CHECK((uint32_t)GCC_VOTE == 0x61010, "the register the votes go into is the board's: %#x", (uint32_t)GCC_VOTE);
	CHECK(strcmp(WIRED->io_supply, "ldob5") == 0, "the supply's name is the board's: %s", WIRED->io_supply);
}

/* a board without touch { }, asked for touch: said, and nothing mapped or touched */
static void
not_wired(void)
{
	board = "qemu-virt";
	boot_args = "touchscreen=1";
	hakios_goodix_init();
	usleep(100 * 1000);
	CHECK(logged("disabled:"), "said: %s", log_text);
	CHECK(mappings == 0 && accesses == 0, "nothing mapped (%u), nothing touched (%u)", mappings, accesses);
}

/* without its boot argument: nothing, on the phone it is for either */
static void
off_by_default(void)
{
	board = "phone-video";
	boot_args = "";
	hakios_goodix_init();
	usleep(100 * 1000);
	CHECK(mappings == 0 && accesses == 0 && log_text[0] == 0, "nothing mapped, touched or said: %s", log_text);
}

/* touchscreen=-1: the clock controller's and the pins' state read and said; nothing written -- no pin, clock, supply or chip */
static void
look_only(void)
{
	board = "phone-video";
	boot_args = "touchscreen=-1";
	hakios_goodix_init();
	usleep(200 * 1000);
	CHECK(logged("the straps read") && logged("nothing powered, configured or sent"), "said: %s", log_text);
	CHECK(accesses > 0 && writes == 0, "read (%u), and nothing written (%u)", accesses, writes);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "as_it_was", as_it_was }, { "another_wiring", another_wiring }, { "not_wired", not_wired }, { "off_by_default", off_by_default },
		{ "look_only", look_only },
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
