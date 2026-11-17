/*
 * The CPU frequency driver itself (drivers/board/cpufreq.c), run as a host program
 * against a model of the frequency block, EPSS's or the OSM's (board/cpufreq_hw.h): a frequency table, a
 * "row wanted" register and an enable bit for each group of cores, where the board's SoC file says they are
 * and laid out as it says. What is tested: every group that has a core of the
 * kernel's is asked for the top row of its own table, a group without one is left alone, and nothing
 * is written on a board that does not list the feature or without the boot argument.
 *
 *   cpufreq_driver_test SCENARIO     one scenario a process: the driver looks its board up once
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the hardware's model ----------------------------------------------------------------------- */

#define GROUPS          4
#define ROWS            40

static struct {
	uint32_t base[GROUPS], size;    /* where the test put each group's block: from the board the driver reads */
	uint32_t table_at, row_size, perf_at;   /* the layout: EPSS's 0x100, 4, 0x320; the OSM's 0x110, 32, 0x920 */
	uint32_t table[GROUPS][ROWS];   /* [31:30] the clock source, [7:0] the multiple of 19.2 MHz */
	uint32_t wanted[GROUPS];
	uint32_t enable[GROUPS];        /* [0]: the hardware is in charge of the group's clock */
	unsigned writes[GROUPS], elsewhere;
} epss;

/* the group whose block ADDRESS is in, and where in it; GROUPS for none */
static unsigned
group_at(vm_offset_t address, uint32_t *offset)
{
	for (unsigned g = 0; g < GROUPS; g++) {
		if (epss.size != 0 && epss.base[g] != 0 && address >= epss.base[g] && address < epss.base[g] + epss.size) {
			*offset = (uint32_t)(address - epss.base[g]);
			return g;
		}
	}
	return GROUPS;
}

/* a group's table: 300 MHz from the fixed source, then the multiples given, then the last repeated to the end */
static void
table(unsigned group, unsigned count, const uint8_t *multiples)
{
	epss.table[group][0] = 0;
	for (unsigned row = 1; row < ROWS; row++) {
		epss.table[group][row] = 1u << 30 | multiples[row - 1 < count ? row - 1 : count - 1];
	}
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t offset = 0;
	unsigned group = group_at(address, &offset);

	if (group < GROUPS) {
		if (offset >= epss.table_at && offset < epss.table_at + epss.row_size * ROWS && (offset - epss.table_at) % epss.row_size == 0) {
			return epss.table[group][(offset - epss.table_at) / epss.row_size];
		}
		if (offset == epss.perf_at) {
			return epss.wanted[group];
		}
		if (offset == 0) {
			return epss.enable[group];
		}
	}
	epss.elsewhere++;
	fprintf(stderr, "MODEL: a read at %#lx, where nothing of the frequency block is\n", (unsigned long)address);
	return 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	uint32_t offset = 0;
	unsigned group = group_at(address, &offset);

	if (group < GROUPS && offset == epss.perf_at) {
		epss.wanted[group] = value;
		epss.writes[group]++;
		return;
	}
	epss.elsewhere++;
	fprintf(stderr, "MODEL: a write of %#x at %#lx, which is not a group's row register\n", value, (unsigned long)address);
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the driver's neighbours, which its start calls (bsd/dev/board/ddr.c, board/bench.c) */
static unsigned load_meters, memory_votes, benchmarks;
static bool vote_with_cpufreq;
void hakios_cpuload_start(void) { load_meters++; }
void hakios_ddr_init(bool with_cpufreq) { memory_votes++; vote_with_cpufreq = with_cpufreq; }
void hakios_membench(void) { benchmarks++; }
void hakios_cpufreq_init(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
/* the kernel's log beside the console (board/input.c): what the parking of the small cores says there is not looked at */
void hakios_tl(const char *format, ...) __printflike(1, 2);
void hakios_tl(const char *format __unused, ...) {}

/*
 * (cpu_gov=0 in every scenario that raises the clocks: the governor board/cpufreq.c starts after them looks at the
 * cores' load in a thread of its own, and here, where no core is ever busy, would take the rows down while a
 * scenario reads them. What is checked is what the driver sets at its start.)
 */
#include "board/board.c"
#include "board/cpufreq.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static unsigned
all_writes(void)
{
	return epss.writes[0] + epss.writes[1] + epss.writes[2] + epss.writes[3];
}

/* a phone: the block where its SoC file has it, each group left at a low row by the boot loader */
static void
phone(const char *name, unsigned cores)
{
	static const uint8_t small[] = { 30, 40, 53, 65, 78, 89, 94 };         /* ... 1804 MHz */
	static const uint8_t big[] = { 37, 49, 63, 80, 94, 104, 114, 125 };    /* ... 2400 MHz */
	static const uint8_t prime[] = { 44, 62, 85, 109, 125, 141 };          /* ... 2707 MHz */
	const struct hakios_board *b;

	board = name;
	fake_cores = cores;
	fake_delay_divisor = 20;
	b = hakios_board();
	/* (the model's layout and places from the board, but written out here: not the driver's own arithmetic) */
	epss.table_at = 0x100, epss.row_size = 4, epss.perf_at = 0x320;
	if (b != NULL && b->cpufreq.block == HAKIOS_CPUFREQ_OSM) {
		epss.table_at = 0x110, epss.row_size = 32, epss.perf_at = 0x920;
	}
	if (b != NULL) {
		epss.size = b->cpufreq.epss_domain_size;
		for (unsigned g = 0; g < GROUPS && b->cpufreq.domain_cores[g] != 0; g++) {
			epss.base[g] = b->cpufreq.domains[g] ? b->cpufreq.domains[g] : b->cpufreq.epss + g * epss.size;
		}
	}
	for (unsigned g = 0; g < GROUPS; g++) {
		epss.enable[g] = 1;
	}
	table(0, sizeof(small), small);
	table(1, sizeof(big), big);
	table(2, sizeof(prime), prime);
	table(3, sizeof(small), small);
	epss.wanted[0] = 2;
	epss.wanted[1] = 1;
	epss.wanted[2] = 1;
	epss.wanted[3] = 3;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* cpufreq=1 on a phone with all eight cores: each of its three groups at the top of its own table */
static void
every_group(void)
{
	phone("phone-cmd", 8);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8 && epss.wanted[2] == 6, "rows %u %u %u", epss.wanted[0], epss.wanted[1],
	    epss.wanted[2]);
	CHECK(epss.writes[0] == 1 && epss.writes[1] == 1 && epss.writes[2] == 1 && epss.writes[3] == 0, "one write a group");
	CHECK(logged("cpufreq: group 0 (4 of our cores): row 2 -> 7 of the frequency table, 1804 MHz"), "the small cores: %s", log_text);
	CHECK(logged("cpufreq: group 1 (3 of our cores): row 1 -> 8 of the frequency table, 2400 MHz"), "the big cores");
	CHECK(logged("cpufreq: group 2 (1 of our cores): row 1 -> 6 of the frequency table, 2707 MHz"), "the biggest");
	CHECK(logged("cpufreq: on core 0 a loop ran at"), "the loop's speed before and after is said");
	CHECK(load_meters == 1 && memory_votes == 1 && vote_with_cpufreq && benchmarks == 1, "the neighbours are started: %u %u %u",
	    load_meters, memory_votes, benchmarks);
	CHECK(epss.elsewhere == 0, "%u accesses where nothing of the block is", epss.elsewhere);
}

/* cpus=1: the kernel has one core, and only that core's group is raised */
static void
one_core(void)
{
	phone("phone-cmd", 1);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(epss.wanted[0] == 7 && epss.writes[0] == 1, "the first group: row %u", epss.wanted[0]);
	CHECK(epss.wanted[1] == 1 && epss.wanted[2] == 1 && epss.writes[1] + epss.writes[2] == 0, "the groups without a core of ours "
	    "are as the boot loader left them (%u %u)", epss.wanted[1], epss.wanted[2]);
	CHECK(logged("group 0 (1 of our cores)") && !logged("group 1") && !logged("group 2"), "said for that group only");
	CHECK(epss.elsewhere == 0, "%u accesses where nothing of the block is", epss.elsewhere);
}

/* five cores: the four small ones and one big one */
static void
some_cores(void)
{
	phone("phone-cmd", 5);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8 && epss.wanted[2] == 1, "rows %u %u %u", epss.wanted[0], epss.wanted[1],
	    epss.wanted[2]);
	CHECK(logged("group 1 (1 of our cores)"), "the big group with the one core the kernel has of it");
}

/* a chip of before EPSS with two groups, six and two cores: the OSM's layout, its blocks elsewhere and not one size apart */
static void
another_soc(void)
{
	phone("phone-keys", 8);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(epss.base[0] == 0x05591000 && epss.base[1] == 0x05593800 && epss.size == 0x1400 && epss.perf_at == 0x920,
	    "the blocks are where that SoC file has them: %#x %#x, %#x", epss.base[0], epss.base[1], epss.size);
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8, "rows %u %u", epss.wanted[0], epss.wanted[1]);
	CHECK(epss.writes[2] + epss.writes[3] == 0, "it has no third group");
	CHECK(logged("group 0 (6 of our cores)") && logged("group 1 (2 of our cores)"), "six and two: %s", log_text);
	CHECK(epss.elsewhere == 0, "%u accesses where nothing of the block is", epss.elsewhere);
}

/* each group's rows' core counts said, and rows of count 1 (Linux's turbo rows) named as such; nothing else changes */
static void
core_counts_said(void)
{
	phone("phone-cmd", 8);
	for (unsigned row = 0; row < ROWS; row++) {
		epss.table[1][row] |= (row >= 6 ? 1u : 3u) << 16;      /* the big group's last rows: one core at a time */
		epss.table[0][row] |= 4u << 16;
	}
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(logged("cpufreq: group 0: the rows' core counts 44444444\n"), "the small group: %s", log_text);
	CHECK(logged("cpufreq: group 1: the rows' core counts 333333111 (1: a row Linux leaves out as turbo)"), "the big one");
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8, "still the top rows: %u %u", epss.wanted[0], epss.wanted[1]);
}

/* a group whose hardware is not in charge of its clock (its enable bit clear) is left alone and said, as Linux leaves it */
static void
not_in_charge(void)
{
	phone("phone-cmd", 8);
	epss.enable[1] = 0;
	boot_args = "cpufreq=1 cpu_gov=0 park_small=0";
	hakios_cpufreq_init();
	CHECK(epss.wanted[1] == 1 && epss.writes[1] == 0, "the big group untouched: row %u, %u writes", epss.wanted[1], epss.writes[1]);
	CHECK(logged("cpufreq: group 1 (3 of our cores): the hardware is not in charge of its clock: left alone"), "said: %s", log_text);
	CHECK(epss.wanted[0] == 7 && epss.wanted[2] == 6, "the others raised: %u %u", epss.wanted[0], epss.wanted[2]);
	boot_args = "";
	gov_start();
	CHECK(gov[1].base == 0 && gov[0].base != 0, "and not governed");
	CHECK(epss.elsewhere == 0, "%u accesses where nothing of the block is", epss.elsewhere);
}

/* a table of one row above the fixed source, one that never repeats, and one of the fixed source alone: the top is still found */
static void
odd_tables(void)
{
	static const uint8_t one[] = { 50 };
	uint8_t rising[ROWS - 1];

	phone("phone-cmd", 8);
	table(0, sizeof(one), one);
	for (unsigned i = 0; i < ROWS - 1; i++) {
		rising[i] = (uint8_t)(20 + i);
	}
	table(1, sizeof(rising), rising);
	for (unsigned row = 0; row < ROWS; row++) {
		epss.table[2][row] = 0;         /* the third group has the fixed 300 MHz and nothing above it */
	}
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(epss.wanted[0] == 1, "a table of one row: row %u", epss.wanted[0]);
	CHECK(logged("row 2 -> 1 of the frequency table, 960 MHz"), "... which is 50 x 19.2 MHz");
	CHECK(epss.wanted[1] == ROWS - 1, "a table that fills every row: row %u", epss.wanted[1]);
	CHECK(epss.wanted[2] == 0 && logged("-> 0 of the frequency table, 300 MHz"), "a table of the fixed source alone: row %u, said as 300 MHz: %s",
	    epss.wanted[2], log_text);
	CHECK(epss.elsewhere == 0, "the table is not read past its end (%u)", epss.elsewhere);
}

/* without the boot argument nothing is written; the memory vote is told that the cores are not raised */
static void
off_by_default(void)
{
	phone("phone-cmd", 8);
	boot_args = "";
	hakios_cpufreq_init();
	CHECK(all_writes() == 0 && !logged("cpufreq:"), "nothing written, nothing said: %s", log_text);
	CHECK(memory_votes == 1 && !vote_with_cpufreq && benchmarks == 0, "the memory is not asked for more either");
	boot_args = "cpufreq=0";
	hakios_cpufreq_init();
	CHECK(all_writes() == 0, "cpufreq=0 is off");
}

/* a board that does not list the feature: said, and nothing touched */
static void
not_listed(void)
{
	phone("qemu-virt", 8);
	for (unsigned g = 0; g < 3; g++) {
		epss.base[g] = 0x18591000 + g * 0x1000;
	}
	epss.size = 0x1000;
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(logged("cpufreq: disabled: this board's board.dts lists no \"cpufreq-epss\""), "said: %s", log_text);
	CHECK(all_writes() == 0 && epss.elsewhere == 0, "nothing read or written");
}

/* a machine nobody described */
static void
unknown_board(void)
{
	phone("nobody-described-this", 8);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	CHECK(logged("cpufreq: disabled"), "said: %s", log_text);
	CHECK(all_writes() == 0 && epss.elsewhere == 0, "nothing read or written");
}

/*
 * The governor (gov_start, gov_look), called here look by look rather than from its thread: the phone's eight
 * cores raised with cpu_gov=0, then the governor started on them as cpufreq=1 starts it. BUSY gives the share
 * of the last look of every core of the big group (cores 4 to 6), the others idle.
 */
static uint32_t busy_of[32];

static void
governed(void)
{
	phone("phone-cmd", 8);
	/* (park_small=0 as well: no thread that parks and looks, which would look too, between this test's looks, once
	 * gov_start() has put the governor on) */
	boot_args = "cpufreq=1 cpu_gov=0 park_small=0";
	hakios_cpufreq_init();
	boot_args = "";
	gov_start();
}

static void
look(uint32_t big_busy)
{
	for (unsigned i = 4; i < 7; i++) {
		busy_of[i] = big_busy;
	}
	gov_look(busy_of);
}

/* each group the kernel has all of is governed, said with its table */
static void
governor_starts(void)
{
	governed();
	CHECK(gov_on && gov[0].base != 0 && gov[1].base != 0 && gov[2].base != 0 && gov[3].base == 0, "the three groups");
	CHECK(gov[1].rows == 9 && gov[1].top == 8 && gov[1].row == 8 && gov[1].cores == 0x70, "the big group: %u rows, at %u, cores %#x",
	    gov[1].rows, gov[1].row, gov[1].cores);
	CHECK(gov[1].khz[0] == 300000 && gov[1].khz[8] == 2400000, "its table: %u to %u kHz", gov[1].khz[0], gov[1].khz[8]);
}

/* a row register that reads past the table (a domain not enabled, a value with other bits): the governor's row is the top, and its
 * looks are counted inside the table -- the row was an index into khz[] and looks_at[] as it was read */
static void
governor_row_past_the_table(void)
{
	phone("phone-cmd", 8);
	boot_args = "cpufreq=1 cpu_gov=0 park_small=0";
	hakios_cpufreq_init();
	boot_args = "";
	epss.wanted[1] = 0x80000037;            /* (what the register says, whatever the kernel wrote) */
	gov_start();
	CHECK(gov[1].row == gov[1].top, "the big group taken at its top row, not at %u", gov[1].row);
	look(5);
	CHECK(gov[1].row <= gov[1].top && gov[1].looks_at[gov[1].row] == 1, "a look counted at row %u", gov[1].row);
}

/* down only after four looks that want it, and then to the most any of them wanted */
static void
governor_goes_down_slowly(void)
{
	governed();
	look(10);                               /* 2400 MHz * 10% * 1.25 = 300 MHz: row 0 */
	look(27);                               /* 810 MHz: row 2 (940.8) */
	look(23);                               /* 690 MHz: row 1 (710.4) */
	CHECK(epss.wanted[1] == 8, "three looks: still at the top: %u", epss.wanted[1]);
	look(10);
	CHECK(epss.wanted[1] == 2, "the fourth: the most of the four, row 2: %u", epss.wanted[1]);
	CHECK(epss.wanted[0] == 0 && epss.wanted[2] == 0, "the idle groups after the same four looks: rows %u %u", epss.wanted[0],
	    epss.wanted[2]);
	look(10);
	look(10);
	look(10);
	CHECK(epss.wanted[1] == 2, "three more: %u", epss.wanted[1]);
	look(10);
	CHECK(epss.wanted[1] == 0, "and four: row 0: %u", epss.wanted[1]);
}

/* up at once: to the row that covers the load, or to the top from 90% */
static void
governor_goes_up_at_once(void)
{
	governed();
	for (int i = 0; i < 8; i++) {
		look(0);
	}
	CHECK(epss.wanted[1] == 0, "idle: row 0: %u", epss.wanted[1]);
	look(89);                               /* 300 MHz * 89% * 1.25 = 333.75 MHz: row 1 */
	CHECK(epss.wanted[1] == 1, "busy: the row that covers it, at once: %u", epss.wanted[1]);
	look(0);
	look(0);
	look(0);
	look(50);                               /* 710.4 MHz * 50% * 1.25 = 444 MHz, row 1 again: the looks down start again */
	look(0);
	look(0);
	look(0);
	CHECK(epss.wanted[1] == 1, "not down: a look at its row came between: %u", epss.wanted[1]);
	look(90);
	CHECK(epss.wanted[1] == 8, "90%%: the top: %u", epss.wanted[1]);
}

/* at the run's end the governed groups at their top rows, and no other written */
static void
governor_quiesced(void)
{
	governed();
	for (int i = 0; i < 8; i++) {
		look(0);
	}
	CHECK(epss.wanted[0] == 0 && epss.wanted[1] == 0 && epss.wanted[2] == 0, "low: %u %u %u", epss.wanted[0], epss.wanted[1],
	    epss.wanted[2]);
	hakios_cpufreq_quiesce();
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8 && epss.wanted[2] == 6 && !gov_on, "at the tops again: %u %u %u",
	    epss.wanted[0], epss.wanted[1], epss.wanted[2]);
	CHECK(epss.writes[3] == 0 && epss.elsewhere == 0, "the fourth group and nothing else: %u, %u", epss.writes[3], epss.elsewhere);
}

/* a group with a core the kernel has not got (the loader's) is not governed */
static void
governor_leaves_a_shared_group(void)
{
	phone("phone-cmd", 5);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	boot_args = "";
	gov_start();
	CHECK(gov[0].base != 0 && gov[1].base == 0 && gov[2].base == 0, "only the small group, all of whose cores are ours");
	look(0);
}

/* thermal=2 holds the rows back itself, and cpu_gov=0 asks for none: no governor */
static void
governor_not_with_thermal(void)
{
	phone("phone-cmd", 8);
	boot_args = "cpufreq=1 cpu_gov=0";
	hakios_cpufreq_init();
	boot_args = "thermal=2";
	gov_start();
	CHECK(!gov_on && gov[1].base == 0, "a group governed");
}

/*
 * The small cores parked (park_small, on by default): the eight-core phone with cores 0 to 3 small, each core
 * as busy as fake_busy_pct says, and the parking thread looking every 0.4 ms (fake_delay_divisor 20). Without
 * cpufreq=1: parking touches no clock.
 */
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) usleep(5000); } while (0)

static uint32_t
recommended(void)
{
	return *(volatile uint32_t *)&fake_recommended_cores;
}

static void
big_cores_busy(unsigned pct)
{
	for (unsigned i = 4; i < 8; i++) {
		fake_busy_pct[i] = pct;
	}
}

static void
parking_phone(const char *arguments, unsigned small)
{
	phone("phone-cmd", 8);
	fake_small_cores = small;
	fake_idle_kept = true;
	big_cores_busy(10);
	boot_args = arguments;
	hakios_cpufreq_init();
}

/* parked from the start, given back when the least busy big core is busy, parked again once all are calm */
static void
small_cores_parked(void)
{
	parking_phone("", 0x0f);
	WAIT_FOR(recommended() == 0xf0, 1000);
	CHECK(recommended() == 0xf0, "the big ones only: %#x", recommended());
	big_cores_busy(95);
	WAIT_FOR(recommended() == 0xff, 1000);
	CHECK(recommended() == 0xff, "busy: every core: %#x", recommended());
	big_cores_busy(70);                     /* between calm (60) and busy (85): kept as it is */
	usleep(200000);
	CHECK(recommended() == 0xff, "not calm yet: %#x", recommended());
	big_cores_busy(30);
	WAIT_FOR(recommended() == 0xf0, 1000);
	CHECK(recommended() == 0xf0, "calm: parked again: %#x", recommended());
	CHECK(all_writes() == 0 && epss.elsewhere == 0, "no clock touched: %u", all_writes());
}

/* one big core idle is enough to keep them parked: it is the least busy that counts */
static void
small_cores_kept_parked(void)
{
	parking_phone("", 0x0f);
	WAIT_FOR(recommended() == 0xf0, 1000);
	big_cores_busy(95);
	fake_busy_pct[7] = 20;
	usleep(200000);
	CHECK(recommended() == 0xf0, "a big core has room: still parked: %#x", recommended());
}

/* park_small=0: never parked */
static void
small_cores_not_parked(void)
{
	parking_phone("park_small=0", 0x0f);
	usleep(200000);
	CHECK(recommended() == ~0u, "the scheduler never told: %#x", recommended());
}

/* a phone whose cores are all alike: nothing to park */
static void
nothing_to_park(void)
{
	parking_phone("", 0);
	usleep(200000);
	CHECK(recommended() == ~0u, "the scheduler never told: %#x", recommended());
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "every_group", every_group }, { "one_core", one_core }, { "some_cores", some_cores }, { "another_soc", another_soc },
		{ "not_in_charge", not_in_charge }, { "core_counts_said", core_counts_said }, { "odd_tables", odd_tables }, { "off_by_default", off_by_default }, { "not_listed", not_listed },
		{ "unknown_board", unknown_board }, { "governor_starts", governor_starts }, { "governor_row_past_the_table", governor_row_past_the_table },
		{ "governor_goes_down_slowly", governor_goes_down_slowly }, { "governor_goes_up_at_once", governor_goes_up_at_once },
		{ "governor_quiesced", governor_quiesced }, { "governor_leaves_a_shared_group", governor_leaves_a_shared_group },
		{ "governor_not_with_thermal", governor_not_with_thermal }, { "small_cores_parked", small_cores_parked },
		{ "small_cores_kept_parked", small_cores_kept_parked }, { "small_cores_not_parked", small_cores_not_parked },
		{ "nothing_to_park", nothing_to_park },
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
