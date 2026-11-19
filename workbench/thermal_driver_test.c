/*
 * The thermal limiter itself (drivers/board/thermal.c),
 * run as a host program on the fake kernel (fakekernel/) against a model of two TSENS controllers and the
 * three EPSS groups of a lahaina-family SoC -- or, for phone-keys, one controller and the two groups of a chip
 * of the OSM's layout, their blocks not one size apart (board/cpufreq_hw.h). workbench/run.sh builds it for the host and for aarch64
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
/* phone-keys' (workbench/devices/phone-keys/soc.dtsi): the OSM's layout -- the table at 0x110, a row every 32 bytes,
 * the row wanted at 0x920, what LMh allows at 0x704 -- and its TSENS */
#define OSM0            0x05591000u
#define OSM1            0x05593800u
#define OSM_TSENS       0x0d263000u
static bool osm;

static uint32_t sensor[2][16];          /* status words */
static uint32_t perf[3];                /* each group's row */
static uint32_t lmh[3];                 /* each group's DOMAIN_STATE: what the hardware allows, times 19.2 MHz */
static uint32_t enabled[3] = { 1, 1, 1 };       /* each group's enable register: the hardware runs its clock */
static char tl_lines[8192];             /* the "tl:" lines the driver wrote */
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
		uint32_t base = osm ? (c ? 0 : OSM_TSENS) : c ? TSENS1 : TSENS0;

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
	if (osm && ((a >= OSM0 && a < OSM0 + 0x1400) || (a >= OSM1 && a < OSM1 + 0x1400))) {
		unsigned d = a >= OSM1, off = (unsigned)(a - (d ? OSM1 : OSM0));

		if (off >= 0x110 && off < 0x110 + 32 * 40 && (off - 0x110) % 32 == 0) {
			unsigned row = (off - 0x110) / 32;

			row = row >= ROWS ? ROWS - 1 : row;
			return table_mult[row] ? 1u << 30 | table_mult[row] : 0;
		}
		if (off == 0x920) {
			return perf[d];
		}
		if (off == 0) {
			return enabled[d];
		}
		if (off == 0x704) {
			return lmh[d] | 0x7000;         /* (above [9:0], something else) */
		}
	}
	if (!osm && a >= EPSS && a < EPSS + 0x3000) {
		unsigned d = (a - EPSS) / 0x1000, off = (a - EPSS) % 0x1000;

		if (off >= 0x100 && off < 0x100 + 4 * 40) {
			unsigned row = (off - 0x100) / 4;

			row = row >= ROWS ? ROWS - 1 : row;
			return table_mult[row] ? 1u << 30 | table_mult[row] : 0;
		}
		if (off == 0x320) {
			return perf[d];
		}
		if (off == 0x20) {
			return lmh[d];
		}
		if (off == 0) {
			return enabled[d];
		}
	}
	foreign++;
	return 0;
}

void
hakios_test_wr(vm_offset_t a, uint32_t v)
{
	if (osm && (a == OSM0 + 0x920 || a == OSM1 + 0x920)) {
		same_again += perf[a == OSM1 + 0x920] == v;
		perf[a == OSM1 + 0x920] = v;
		writes++;
		return;
	}
	if (!osm && a >= EPSS && a < EPSS + 0x3000 && (a - EPSS) % 0x1000 == 0x320) {
		same_again += perf[(a - EPSS) / 0x1000] == v;
		perf[(a - EPSS) / 0x1000] = v;
		writes++;
		return;
	}
	foreign++;
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_backlight_set_cap(uint32_t level) { cap = level; }
uint32_t hakios_adreno_core_mhz(void) { return 550; }

void
hakios_tl(const char *format, ...)
{
	va_list list;
	size_t at = strlen(tl_lines);

	va_start(list, format);
	if (at + 200 < sizeof(tl_lines)) {
		vsnprintf(tl_lines + at, sizeof(tl_lines) - at, format, list);
	}
	va_end(list);
}
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
	lmh[0] = lmh[1] = lmh[2] = 100;             /* the hardware allows the top row: cool */
}

/* with no boot-arg it looks: the hottest read, a line a look with what the hardware allows, nothing written */
static void
looks_by_default(void)
{
	phone(900);
	boot_args = "th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("hottest") == 900, 1000);
	CHECK((int32_t)get("hottest") == 900, "%d", (int32_t)get("hottest"));
	usleep(300000);
	CHECK(writes == 0 && foreign == 0 && perf[0] == ROWS - 1, "nothing written, nothing else read");
	CHECK(strstr(tl_lines, "tl: h at ") != NULL && strstr(tl_lines, " hottest 900 sensor 3 limit 1920,1920,1920 asked 1920,1920,1920 gpu 550\n"),
	    "the line: %.200s", tl_lines);
}

/* thermal=0: nothing read at all */
static void
off_when_asked(void)
{
	phone(900);
	boot_args = "thermal=0";
	hakios_thermal_init();
	usleep(100000);
	CHECK(writes == 0 && foreign == 0 && perf[0] == ROWS - 1 && tl_lines[0] == 0, "nothing touched");
}

/* the SoC's own limits hardware slowing a group: its line says so, below the row asked for; and the hottest is the
 * second controller's sensor 5 (16 + 5) */
static void
throttled_by_the_hardware(void)
{
	phone(600);
	temperature(1, 5, 820);
	lmh[1] = 63;                            /* 1209.6 MHz */
	boot_args = "thermal=1 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(strstr(tl_lines, "limit") != NULL, 1000);
	CHECK(strstr(tl_lines, " hottest 820 sensor 21 limit 1920,1209,1920 asked 1920,1920,1920 ") != NULL, "the line: %.200s", tl_lines);
	CHECK(writes == 0, "looking only: nothing written");
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

/* a chip of the OSM's layout (SDM845 to SM8250), its two groups' blocks not one size apart: looked at and held back the same */
static void
osm_chip(void)
{
	phone(600);
	board = "phone-keys";
	osm = true;
	lmh[1] = 600;                           /* 11520 MHz: [9:0] counts, not [7:0] */
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(strstr(tl_lines, "limit") != NULL, 1000);
	CHECK(strstr(tl_lines, " hottest 600 sensor 3 limit 1920,11520 asked 1920,1920 ") != NULL, "the line: %.200s", tl_lines);
	CHECK(get("most") == 5, "the small group from row 8 to its floor row 3: %u", get("most"));
	temperature(0, 3, 720);
	WAIT_FOR(get("step") == 5 && perf[0] == 3 && perf[1] == 4, 3000);
	CHECK(perf[0] == 3 && perf[1] == 4, "at the floors, written at 0x920 of each block: %u %u", perf[0], perf[1]);
	temperature(0, 3, 550);
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1 && perf[1] == ROWS - 1, 3000);
	CHECK(perf[0] == ROWS - 1 && perf[1] == ROWS - 1, "given back: %u %u", perf[0], perf[1]);
	CHECK(foreign == 0, "no other register: %u", foreign);
}

/* a group whose hardware is not in charge of its clock is looked at and never written, as board/cpufreq.c leaves it */
static void
group_not_in_charge(void)
{
	phone(500);
	enabled[1] = 0;
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(logged("thermal: group 1: the hardware is not in charge of its clock: looked at, never held back"), "said: %s", log_text);
	phone(720);
	enabled[1] = 0;
	WAIT_FOR(get("step") == 5 && perf[0] == 3 && perf[2] == 4, 3000);
	CHECK(perf[0] == 3 && perf[2] == 4, "the others held back: %u %u", perf[0], perf[2]);
	CHECK(perf[1] == ROWS - 1, "the group that is not the hardware's untouched: row %u", perf[1]);
	phone(550);
	enabled[1] = 0;
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1 && perf[2] == ROWS - 1, 3000);
	CHECK(perf[1] == ROWS - 1, "and still untouched once the others are given back: row %u", perf[1]);
	CHECK(foreign == 0, "no other register: %u", foreign);
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

/* a jump straight to the most held back, refused once by the firmware: the step does not change again, and the
 * levels are still asked for at the next looks until they are taken */
static void
scmi_refused_at_critical(void)
{
	scmi_phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	usleep(200000);
	scmi_phone(960);
	scmi_refusals = 2;
	WAIT_FOR(get("step") == 9 && scmi_level[0] == 2 && scmi_level[1] == 3, 2000);
	CHECK(get("step") == 9, "critical: the most held back: %u", get("step"));
	CHECK(scmi_level[0] == 2 && scmi_level[1] == 3, "refused once, asked again and taken: %u %u", scmi_level[0], scmi_level[1]);
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

/* a row register that reads past the table: the group's top is the table's last row, and no row past it is ever written */
static void
row_past_the_table(void)
{
	phone(500);
	perf[0] = 0x80000037;
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(get("most") == 5, "held back from the table's last row, 8, to the floor, 3: %u", get("most"));
	phone(800);
	perf[0] = 0x80000037;
	WAIT_FOR(get("step") >= 1, 2000);
	usleep(200000);
	CHECK(perf[0] < ROWS, "the rows written are the table's: %#x", perf[0]);
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

/*
 * Every sensor of a phone held back because it is hot goes silent (nonsense or nothing): not knowing how hot it is,
 * nothing is given back -- the rows stay where they were held, look after look -- until a sensor answers again and
 * says it has cooled
 */
static void
silent_sensors(void)
{
	uint32_t held, rows[3];

	phone(720);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("step") >= 3, 2000);
	for (unsigned c = 0; c < 2; c++) {
		for (unsigned s = 0; s < 16; s++) {
			sensor[c][s] = s & 1 ? (1u << 21) | 0x7ff : 0;     /* nonsense or nothing */
		}
	}
	WAIT_FOR((int32_t)get("hottest") == TH_NONE, 2000);
	usleep(150000);                         /* (the look that saw the last reading is done) */
	held = get("step");
	rows[0] = perf[0], rows[1] = perf[1], rows[2] = perf[2];
	usleep(800000);                         /* eight looks or so without a reading */
	CHECK((int32_t)get("hottest") == TH_NONE, "%d", (int32_t)get("hottest"));
	CHECK(held >= 3 && get("step") == held, "no reading is no reason to give back: step %u, was %u", get("step"), held);
	CHECK(perf[0] == rows[0] && perf[1] == rows[1] && perf[2] == rows[2] && perf[0] < ROWS - 1, "the rows held: %u %u %u",
	    perf[0], perf[1], perf[2]);
	temperature(0, 3, 500);                 /* the sensors answer again: cool (the rows left as the driver wrote them) */
	temperature(1, 1, 450);
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1, 3000);
	CHECK(get("step") == 0 && perf[0] == ROWS - 1, "cool and said so: given back: step %u, row %u", get("step"), perf[0]);
}

/*
 * The sensor the phone is held back for goes silent while another, cooler one still answers: the cooler one's word is
 * not taken for the silent one's -- nothing is given back until the silent one answers again and says it has cooled
 */
static void
hottest_sensor_silent(void)
{
	uint32_t held;

	phone(720);                             /* 72.0 C, the other sensor 67.0 C */
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("step") >= 3, 2000);
	sensor[0][3] = 0;                       /* the hot one: no reading */
	temperature(1, 1, 550);                 /* the other: 55.0 C, under the clear */
	WAIT_FOR((int32_t)get("hottest") == 550, 1000);
	usleep(150000);
	held = get("step");
	usleep(600000);
	CHECK((int32_t)get("hottest") == 550, "the other sensor's reading: %d", (int32_t)get("hottest"));
	CHECK(held >= 3 && get("step") == held, "the hot one silent: nothing given back: step %u, was %u", get("step"), held);
	CHECK(logged("thermal: 1 sensor that answered before is silent: held back"), "why it is held is said: %s", log_text);
	temperature(1, 1, 960);                 /* the other one critical meanwhile: held back all the way all the same */
	WAIT_FOR(get("step") == 5, 1000);
	CHECK(get("step") == 5, "a hot one that answers still counts: %u", get("step"));
	temperature(1, 1, 550);
	temperature(0, 3, 550);                 /* the silent one answers again: cool */
	WAIT_FOR(get("step") == 0 && perf[0] == ROWS - 1, 3000);
	CHECK(get("step") == 0 && perf[0] == ROWS - 1, "both cool and said so: given back: step %u, row %u", get("step"), perf[0]);
}

/* how many times the driver has said `text` */
static unsigned
times_logged(const char *text)
{
	unsigned n = 0;

	for (const char *at = strstr(log_text, text); at != NULL; at = strstr(at + 1, text)) {
		n++;
	}
	return n;
}

/*
 * Why it is held is said once each time a silence begins: not again at every look while it lasts -- nor after a hot
 * sensor that answers has held it all the way meanwhile -- and again when a sensor goes silent once every one had
 * answered and it had been given back
 */
static void
silence_said_once_each_time(void)
{
	const char *why = "1 sensor that answered before is silent: held back";

	phone(720);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(get("step") >= 3, 2000);
	sensor[0][3] = 0;                       /* the hot one: no reading */
	temperature(1, 1, 550);                 /* the other: under the clear */
	WAIT_FOR(logged(why), 1000);
	usleep(500000);                         /* five looks or so of it */
	CHECK(times_logged(why) == 1, "said once, not at every look: %u times: %s", times_logged(why), log_text);
	temperature(1, 1, 960);                 /* the other critical: all the way, and kept there a few looks */
	WAIT_FOR(get("step") == 5, 1000);
	usleep(300000);
	temperature(1, 1, 550);                 /* cool again, the hot one still silent */
	usleep(500000);
	CHECK(get("step") == 5, "still silent: nothing given back: %u", get("step"));
	CHECK(times_logged(why) == 1, "the same silence: still said once: %u times", times_logged(why));
	temperature(0, 3, 550);                 /* it answers: cool -- given back */
	WAIT_FOR(get("step") == 0, 3000);
	CHECK(get("step") == 0, "every one answers, cool: given back: %u", get("step"));
	temperature(0, 3, 720);                 /* hot again, then silent again: a silence of its own, said again */
	WAIT_FOR(get("step") >= 3, 2000);
	sensor[0][3] = 0;
	WAIT_FOR(times_logged(why) == 2, 1000);
	CHECK(times_logged(why) == 2, "a second silence is said too: %u times", times_logged(why));
	CHECK(get("step") >= 3, "and held: %u", get("step"));
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

/* root may not move a threshold out of range -- a critical of 150 C would be no critical step at all -- and three
 * tuned out of order, for a moment, leave the last good policy in force */
static void
thresholds_held_to_sense(void)
{
	phone(685);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(try_as_root("critical", 1500) == EINVAL && get("critical") == 950, "critical 150 C refused: %u", get("critical"));
	CHECK(try_as_root("trip", -5) == EINVAL && get("trip") == 700, "a negative trip refused: %u", get("trip"));
	CHECK(try_as_root("critical", 900) == 0 && get("critical") == 900, "a lower critical taken: %u", get("critical"));
	/* 68.5 C; trip lowered to 68 C while clear is still over it at 69 C: until clear is moved too, the policy in
	 * force is the last one in order (trip 70 C), and nothing is held back */
	set_as_root("clear", 690);
	set_as_root("trip", 680);
	usleep(400000);
	CHECK(get("step") == 0, "out of order: the last good policy holds: %u", get("step"));
	set_as_root("clear", 600);
	WAIT_FOR(get("step") >= 1, 2000);
	CHECK(get("step") >= 1, "in order again (trip 68 C): held back: %u", get("step"));
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

/* the hottest said in the log at the first look and every tenth after, not at every one */
static void
said_every_tenth_look(void)
{
	phone(900);
	boot_args = "thermal=1 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(logged("thermal: hottest sensor 90.0 C (thermal=1: looking only)"), 1000);
	CHECK(logged("thermal: hottest sensor 90.0 C (thermal=1: looking only)"), "the first look said: %s", log_text);
	temperature(0, 3, 910);
	usleep(450000);                         /* four or five looks more: none of them the tenth */
	CHECK(!logged("hottest sensor 91.0 C"), "not every look is said: %s", log_text);
	WAIT_FOR(logged("hottest sensor 91.0 C"), 1500);
	CHECK(logged("hottest sensor 91.0 C"), "the tenth is: %s", log_text);
}

/* a phone left out in the frost, -0.5 C its hottest: said with its sign, not as half a degree above freezing */
static void
below_freezing_said(void)
{
	phone(-5);                              /* the other sensor at -5.5 C */
	boot_args = "thermal=1 th_period_ms=100";
	hakios_thermal_init();
	WAIT_FOR(logged("thermal: hottest sensor"), 1000);
	CHECK((int32_t)get("hottest") == -5, "%d", (int32_t)get("hottest"));
	CHECK(logged("thermal: hottest sensor -0.5 C (thermal=1: looking only)"), "said: %s", log_text);
	CHECK(!logged("hottest sensor 0.5 C"), "not as above freezing: %s", log_text);
}

/* what the GPU's driver asks: how far the processors are held back, and of how many steps */
static void
step_for_the_gpu(void)
{
	uint32_t of = 99;

	phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(hakios_thermal_step(&of) == 0 && of == 5, "cool: 0 of 5, not %u of %u", hakios_thermal_step(NULL), of);
	CHECK(!logged("looking only"), "thermal=2 does not say it only looks: %s", log_text);
	phone(960);
	WAIT_FOR(hakios_thermal_step(NULL) == 5, 1000);
	CHECK(hakios_thermal_step(&of) == 5 && of == 5, "hot: 5 of 5, not %u of %u", hakios_thermal_step(NULL), of);
}

/* thermal=2 on a chip without EPSS whose firmware gives no SCMI levels: looking only, and said */
static void
scmi_without_levels(void)
{
	scmi_phone(900);
	scmi_domains = 0;
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	CHECK(logged("SCMI gave no levels") && logged("(thermal=1: looking only)"), "said: %s", log_text);
	usleep(300000);
	CHECK(get("step") == 0 && scmi_asked == 0 && writes == 0, "90 C, and nothing held back or asked: step %u, %u asked",
	    get("step"), scmi_asked);
}

/* at the run's end on a phone that never got hot: nothing written, nothing said */
static void
quiesce_when_cool(void)
{
	phone(500);
	boot_args = "thermal=2 th_period_ms=100";
	hakios_thermal_init();
	usleep(300000);
	hakios_thermal_quiesce();
	CHECK(writes == 0 && !logged("at the run's end"), "%u writes: %s", writes, log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "looks_by_default", looks_by_default }, { "off_when_asked", off_when_asked },
		{ "throttled_by_the_hardware", throttled_by_the_hardware }, { "look_only", look_only }, { "heat_and_cool", heat_and_cool },
		{ "osm_chip", osm_chip }, { "group_not_in_charge", group_not_in_charge },
		{ "critical", critical }, { "never_raises", never_raises }, { "row_past_the_table", row_past_the_table }, { "sensor_between_readings", sensor_between_readings }, { "silent_sensors", silent_sensors }, { "hottest_sensor_silent", hottest_sensor_silent }, { "silence_said_once_each_time", silence_said_once_each_time },
		{ "tuned_while_running", tuned_while_running }, { "thresholds_held_to_sense", thresholds_held_to_sense }, { "unknown_board", unknown_board }, { "board_without_thermal", board_without_thermal },
		{ "bad_policy", bad_policy }, { "clear_at_the_trip", clear_at_the_trip }, { "said_off", said_off },
		{ "slowest_period", slowest_period }, { "too_short_a_period", too_short_a_period }, { "scmi_heat_and_cool", scmi_heat_and_cool }, { "scmi_refused", scmi_refused }, { "scmi_refused_at_critical", scmi_refused_at_critical },
		{ "said_every_tenth_look", said_every_tenth_look }, { "below_freezing_said", below_freezing_said }, { "step_for_the_gpu", step_for_the_gpu },
		{ "scmi_without_levels", scmi_without_levels }, { "quiesce_when_cool", quiesce_when_cool },
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
