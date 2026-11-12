/*
 * The thermal limiter itself (kernel/overlay/bsd/dev/hakios_thermal.c),
 * run as a host program on the fake kernel (fakekernel/) against a model of two TSENS controllers and the
 * three EPSS groups of a lahaina-family SoC. tests/run.sh builds it for the host and for aarch64
 * under qemu-aarch64.
 *
 *   thermal_driver_test SCENARIO
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the hardware's model ----------------------------------------------------------------------- */

#define TSENS0          0x0c263000u
#define TSENS1          0x0c265000u
#define EPSS            0x18591000u
#define ROWS            9

static uint32_t sensor[2][16];          /* status words */
static uint32_t perf[3];                /* each group's row */
static unsigned writes, foreign;
static volatile uint32_t cap = 1000;

/* the frequency table: 300 MHz (alternate source), then multiples of 19.2 MHz, then the last again */
static const uint32_t table_mult[ROWS] = { 0, 30, 40, 53, 65, 78, 89, 94, 100 };   /* 576 ... 1920 MHz */

static void
temperature(unsigned controller, unsigned which, int tenths)
{
	sensor[controller][which] = (1u << 21) | ((uint32_t)tenths & 0xfff);
}

uint32_t
hakios_test_rd(vm_offset_t a)
{
	if (a >= TSENS0 && a < TSENS0 + 0x200) {
		return a >= TSENS0 + 0xa0 && a < TSENS0 + 0xe0 ? sensor[0][(a - TSENS0 - 0xa0) / 4] : 0;
	}
	if (a >= TSENS1 && a < TSENS1 + 0x200) {
		return a >= TSENS1 + 0xa0 && a < TSENS1 + 0xe0 ? sensor[1][(a - TSENS1 - 0xa0) / 4] : 0;
	}
	if (a >= EPSS && a < EPSS + 0x3000) {
		unsigned d = (a - EPSS) / 0x1000, off = (a - EPSS) % 0x1000;

		if (off >= 0x100 && off < 0x100 + 4 * 40) {
			unsigned row = (off - 0x100) / 4;

			row = row >= ROWS ? ROWS - 1 : row;
			return table_mult[row] ? 1u << 30 | table_mult[row] : 0;
		}
		if (off == 0x320) {
			return perf[d];
		}
	}
	foreign++;
	return 0;
}

void
hakios_test_wr(vm_offset_t a, uint32_t v)
{
	if (a >= EPSS && a < EPSS + 0x3000 && (a - EPSS) % 0x1000 == 0x320) {
		perf[(a - EPSS) / 0x1000] = v;
		writes++;
		return;
	}
	foreign++;
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_backlight_set_cap(uint32_t level) { cap = level; }

#include "hakios_board.c"
#define HAKIOS_MMIO_TEST 1
#include "hakios_thermal.c"

/* ---- the scenarios ------------------------------------------------------------------------------ */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) usleep(5000); } while (0)

/* the floors of the test phone, 1000/1100/1100 MHz: the first row at or above, rows 3 (1017.6) and 4 (1248) */
static void
phone(int tenths)
{
	board = "phone-cmd";
	for (unsigned c = 0; c < 2; c++) {
		for (unsigned s = 0; s < 16; s++) {
			sensor[c][s] = 0;               /* not wired up */
		}
	}
	temperature(0, 3, tenths);
	temperature(1, 1, tenths - 50);
	perf[0] = perf[1] = perf[2] = ROWS - 1;      /* (cpufreq=1 has raised them) */
}

static void
off_by_default(void)
{
	phone(900);
	boot_args = "";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0 && perf[0] == ROWS - 1, "nothing touched");
}

static void
look_only(void)
{
	phone(900);
	boot_args = "thermal=1 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("hottest") == 900, 1000);
	CHECK((int32_t)get("hottest") == 900, "%d", (int32_t)get("hottest"));
	usleep(400000);
	CHECK(writes == 0 && perf[1] == ROWS - 1, "never held back");
	CHECK(logged("looking only"), "said so");
	CHECK(foreign == 0, "no other register");
}

static void
heat_and_cool(void)
{
	phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(get("most") == 5, "the small group from row 8 to its floor row 3: %u", get("most"));
	usleep(300000);
	CHECK(get("step") == 0 && writes == 0 && cap == 1000, "cool: nothing");
	phone(720);
	WAIT_FOR(get("step") >= 2, 2000);
	CHECK(perf[0] <= ROWS - 3 && perf[0] >= 3, "a step a look: row %u", perf[0]);
	/* (the step is counted, then the groups' rows and the panel's limit follow it: waited for all of them) */
	WAIT_FOR(get("step") == 5 && perf[0] == 3 && perf[1] == 4 && perf[2] == 4 && cap == 600, 3000);
	CHECK(perf[0] == 3 && perf[1] == 4 && perf[2] == 4, "at the floors: %u %u %u", perf[0], perf[1], perf[2]);
	CHECK(cap == 600, "the panel held back too: %u", cap);
	phone(660);                            /* between clear and trip */
	usleep(500000);
	CHECK(get("step") == 5, "held: %u", get("step"));
	phone(550);
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1 && perf[1] == ROWS - 1 && perf[2] == ROWS - 1 && cap == 1000, 3000);
	CHECK(perf[0] == ROWS - 1 && perf[1] == ROWS - 1 && perf[2] == ROWS - 1, "all given back: %u %u %u", perf[0], perf[1],
	    perf[2]);
	CHECK(cap == 1000, "the panel too: %u", cap);
	CHECK(foreign == 0, "no other register");
}

static void
critical(void)
{
	phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	usleep(200000);
	phone(960);
	WAIT_FOR(get("step") == 5 && perf[0] == 3 && perf[1] == 4, 500);
	CHECK(get("step") == 5 && perf[0] == 3 && perf[1] == 4, "all the way at once: step %u", get("step"));
	hakios_thermal_quiesce();
	CHECK(perf[0] == ROWS - 1 && perf[1] == ROWS - 1, "the loader gets them back: %u %u", perf[0], perf[1]);
}

static void
never_raises(void)
{
	phone(500);
	perf[0] = perf[1] = perf[2] = 0;        /* cpufreq=0: 300 MHz */
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	phone(800);
	perf[0] = perf[1] = perf[2] = 0;
	usleep(500000);
	CHECK(perf[0] == 0 && perf[1] == 0 && perf[2] == 0 && writes == 0, "rows %u %u %u, %u writes", perf[0], perf[1], perf[2],
	    writes);
	CHECK(get("most") == 0, "nothing to hold back: %u", get("most"));
}

static void
silent_sensors(void)
{
	phone(720);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("step") >= 3, 2000);
	for (unsigned c = 0; c < 2; c++) {
		for (unsigned s = 0; s < 16; s++) {
			sensor[c][s] = s & 1 ? (1u << 21) | 0x7ff : 0;     /* nonsense or nothing */
		}
	}
	WAIT_FOR(get("step") == 0 && (int32_t)get("hottest") == TH_NONE, 3000);
	CHECK(get("step") == 0, "no reading is no reason to stay slow: %u", get("step"));
	CHECK((int32_t)get("hottest") == TH_NONE, "%d", (int32_t)get("hottest"));
}

static void
tuned_while_running(void)
{
	phone(650);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	usleep(300000);
	CHECK(get("step") == 0, "65 C under the default trip");
	set_as_root("trip", 600);
	set_as_root("clear", 550);
	WAIT_FOR(get("step") >= 1, 1000);
	CHECK(get("step") >= 1, "a lower trip applies at once: %u", get("step"));
}

static void
unknown_board(void)
{
	phone(900);
	board = "some-new-phone";
	boot_args = "thermal=2";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0, "nothing touched");
	CHECK(logged("no [thermal]"), "said so");
}

/* a board the table has, but without [thermal] (QEMU): the same */
static void
board_without_thermal(void)
{
	phone(900);
	board = "qemu-virt";
	boot_args = "thermal=2";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0, "nothing touched");
	CHECK(logged("no [thermal]"), "said so");
}

static void
bad_policy(void)
{
	phone(500);
	boot_args = "thermal=2 th_trip=900 th_clear=950 th_period_ms=100";
	hakios_thermal_init();
	CHECK(logged("the defaults"), "a clear above the trip is refused");
	CHECK((int32_t)get("trip") == 700 && (int32_t)get("clear") == 620, "%d %d", (int32_t)get("trip"), (int32_t)get("clear"));
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "heat_and_cool", heat_and_cool },
		{ "critical", critical }, { "never_raises", never_raises }, { "silent_sensors", silent_sensors },
		{ "tuned_while_running", tuned_while_running }, { "unknown_board", unknown_board }, { "board_without_thermal", board_without_thermal },
		{ "bad_policy", bad_policy },
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
			printf("%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			if (failures) {
				fputs(log_text, stdout);
			}
			return failures != 0;
		}
	}
	return 2;
}
