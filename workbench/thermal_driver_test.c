/*
 * The thermal limiter itself (kernel/overlay/bsd/dev/board/thermal.c),
 * run as a host program on the fake kernel (fakekernel/) against a model of two TSENS controllers and the
 * three EPSS groups of a lahaina-family SoC. workbench/run.sh builds it for the host and for aarch64
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
static unsigned same_again;             /* writes of the row a group already had */
static unsigned between[2][16];         /* a sensor caught between two readings: this many reads in a row give none */
static unsigned asked[2][16];
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
	for (unsigned c = 0; c < 2; c++) {
		uint32_t base = c ? TSENS1 : TSENS0;

		if (a >= base && a < base + 0x200) {
			unsigned which = (unsigned)(a - base - 0xa0) / 4;

			if (a < base + 0xa0 || a >= base + 0xe0) {
				return 0;
			}
			/* (of every between + 1 reads the last has the reading) */
			if (between[c][which] != 0 && asked[c][which]++ % (between[c][which] + 1) != between[c][which]) {
				return 0;
			}
			return sensor[c][which];
		}
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
		same_again += perf[(a - EPSS) / 0x1000] == v;
		perf[(a - EPSS) / 0x1000] = v;
		writes++;
		return;
	}
	foreign++;
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_backlight_set_cap(uint32_t level) { cap = level; }
/*
 * The firmware's SCMI performance domains: none unless a scenario gives some (the EPSS boards never ask);
 * each with the level it was found at and its top, and the level last set. scmi_refusals: so many of the
 * next sets are refused.
 */
static unsigned scmi_asked, scmi_domains, scmi_refusals;
static uint32_t scmi_found[3], scmi_top[3];
static volatile uint32_t scmi_level[3];
int
hakios_scmi_perf_domain(unsigned domain, uint32_t *found, uint32_t *top)
{
	if (domain >= scmi_domains) {
		return 0;
	}
	*found = scmi_found[domain];
	*top = scmi_top[domain];
	return 1;
}
int
hakios_scmi_perf_set(unsigned domain, uint32_t level)
{
	scmi_asked++;
	if (domain >= scmi_domains || scmi_refusals > 0) {
		scmi_refusals -= scmi_refusals > 0;
		return -1;
	}
	scmi_level[domain] = level;
	return 0;
}

#include "board/board.c"
#define HAKIOS_MMIO_TEST 1
#include "board/thermal.c"

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
	WAIT_FOR(logged("held back 5 of 5 (rows 3 4 4)"), 1000);
	CHECK(logged("thermal: 72.0 C: held back 1 of 5 (rows 7 7 7)") && logged("held back 5 of 5 (rows 3 4 4)"), "each step is said: %s", log_text);
	/* the two groups whose floor is a row higher are at it one step before the first: not written again */
	CHECK(writes == 5 + 4 + 4 && same_again == 0, "a group is written when its row changes: %u writes, %u of the row it had", writes, same_again);
	phone(660);                            /* between clear and trip */
	usleep(500000);
	CHECK(get("step") == 5, "held: %u", get("step"));
	phone(550);
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1 && perf[1] == ROWS - 1 && perf[2] == ROWS - 1 && cap == 1000, 3000);
	CHECK(perf[0] == ROWS - 1 && perf[1] == ROWS - 1 && perf[2] == ROWS - 1, "all given back: %u %u %u", perf[0], perf[1],
	    perf[2]);
	CHECK(cap == 1000, "the panel too: %u", cap);
	CHECK(foreign == 0, "no other register");
	CHECK(scmi_asked == 0, "an EPSS board never asks SCMI: %u", scmi_asked);
}

/* a chip without EPSS: the groups are the firmware's SCMI domains, held back from their top to where they were found */
static void
scmi_phone(int tenths)
{
	phone(tenths);
	board = "phone-scmi";
	scmi_domains = 2;
	scmi_found[0] = 2;
	scmi_top[0] = 10;
	scmi_found[1] = 3;
	scmi_top[1] = 12;
	scmi_level[0] = 10;
	scmi_level[1] = 12;
}

static void
scmi_heat_and_cool(void)
{
	scmi_phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(get("most") == 9, "the second domain from its top 12 to 3: %u", get("most"));
	usleep(300000);
	CHECK(get("step") == 0 && scmi_asked == 0 && writes == 0, "cool: nothing asked");
	scmi_phone(720);
	WAIT_FOR(get("step") == 5 && scmi_level[0] == 2 && scmi_level[1] == 3, 4000);
	CHECK(scmi_level[0] == 2 && scmi_level[1] == 3, "hot: each at the level it was found at: %u %u", scmi_level[0], scmi_level[1]);
	scmi_phone(550);
	WAIT_FOR(get("step") == 0 && scmi_level[0] == 10 && scmi_level[1] == 12, 4000);
	CHECK(scmi_level[0] == 10 && scmi_level[1] == 12, "cool again: at their tops: %u %u", scmi_level[0], scmi_level[1]);
	CHECK(writes == 0 && foreign == 0, "no register of EPSS written: %u", writes);
}

/* a level the firmware refuses is asked for again, at the next look */
static void
scmi_refused(void)
{
	scmi_phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	scmi_refusals = 3;
	scmi_phone(720);
	scmi_refusals = 3;
	WAIT_FOR(get("step") == 5 && scmi_level[0] == 2 && scmi_level[1] == 3, 4000);
	CHECK(scmi_level[0] == 2 && scmi_level[1] == 3, "refused three times, then taken: %u %u", scmi_level[0], scmi_level[1]);
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

/* a sensor caught between two readings has none for a moment: it is asked again, not taken for absent */
static void
sensor_between_readings(void)
{
	phone(900);                             /* the hottest is 90.0 C, the other 85.0 C */
	between[0][3] = 3;                      /* the hottest answers every fourth time it is asked */
	boot_args = "thermal=1 th_period_ms=100";
	hakios_thermal_init();
	usleep(500000);
	CHECK((int32_t)get("hottest") == 900, "the hottest is still seen: %d", (int32_t)get("hottest"));
	between[0][3] = 5;                      /* ... and one that does not answer in four tries is left out that time */
	WAIT_FOR((int32_t)get("hottest") == 850, 2000);
	CHECK((int32_t)get("hottest") == 850, "the other sensor's reading then: %d", (int32_t)get("hottest"));
	CHECK(writes == 0 && foreign == 0, "looking only");
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
	CHECK(logged("no thermal { }"), "said so");
}

/* a board the table has, but without thermal { } (QEMU): the same */
static void
board_without_thermal(void)
{
	phone(900);
	board = "qemu-virt";
	boot_args = "thermal=2";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0, "nothing touched");
	CHECK(logged("no thermal { }"), "said so");
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

/* a clear no lower than the trip: as bad */
static void
clear_at_the_trip(void)
{
	phone(500);
	boot_args = "thermal=2 th_trip=800 th_clear=800 th_period_ms=100";
	hakios_thermal_init();
	CHECK(logged("the defaults") && (int32_t)get("trip") == 700, "a clear at the trip is refused: %s", log_text);
}

/* thermal=0, said: as if not said */
static void
said_off(void)
{
	phone(900);
	boot_args = "thermal=0";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0 && !logged("thermal:"), "nothing touched or said: %s", log_text);
}

/* how often it looks: a reading taken at the start, and the sensor hotter right after; is it read again soon? */
static bool
read_again_within(const char *boot, unsigned ms)
{
	phone(900);
	boot_args = boot;
	hakios_thermal_init();
	WAIT_FOR(get("hottest") == 900, 1000);
	temperature(0, 3, 950);
	usleep(ms * 1000);
	return get("hottest") == 950;
}

/* th_period_ms=10000, the longest: not read again for ten seconds */
static void
slowest_period(void)
{
	CHECK(!read_again_within("thermal=1 th_period_ms=10000", 1500), "not read again in 1.5 s: %d", (int32_t)get("hottest"));
}

/* th_period_ms=50, shorter than the shortest: a second, as without it */
static void
too_short_a_period(void)
{
	CHECK(!read_again_within("thermal=1 th_period_ms=50", 300), "not read again in 0.3 s: %d", (int32_t)get("hottest"));
	usleep(1000000);
	CHECK(get("hottest") == 950, "read again within the second after: %d", (int32_t)get("hottest"));
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "heat_and_cool", heat_and_cool },
		{ "critical", critical }, { "never_raises", never_raises }, { "sensor_between_readings", sensor_between_readings }, { "silent_sensors", silent_sensors },
		{ "tuned_while_running", tuned_while_running }, { "unknown_board", unknown_board }, { "board_without_thermal", board_without_thermal },
		{ "bad_policy", bad_policy }, { "clear_at_the_trip", clear_at_the_trip }, { "said_off", said_off },
		{ "slowest_period", slowest_period }, { "too_short_a_period", too_short_a_period }, { "scmi_heat_and_cool", scmi_heat_and_cool }, { "scmi_refused", scmi_refused },
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
