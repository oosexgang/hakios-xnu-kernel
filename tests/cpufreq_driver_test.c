/*
 * The CPU frequency driver itself (kernel/overlay/bsd/dev/hakios_cpufreq.c), run as a host program
 * against a model of the EPSS block: a frequency table and a "row wanted" register for each group of
 * cores, where the board's SoC file says they are. What is tested: every group that has a core of the
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
	uint32_t base, size;            /* where the test put the block: from the board the driver reads */
	uint32_t table[GROUPS][ROWS];   /* [31:30] the clock source, [7:0] the multiple of 19.2 MHz */
	uint32_t wanted[GROUPS];
	unsigned writes[GROUPS], elsewhere;
} epss;

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
	unsigned group = epss.size ? (unsigned)((address - epss.base) / epss.size) : GROUPS;
	uint32_t offset = epss.size ? (uint32_t)((address - epss.base) % epss.size) : 0;

	if (address >= epss.base && group < GROUPS) {
		if (offset >= 0x100 && offset < 0x100 + 4 * ROWS) {
			return epss.table[group][(offset - 0x100) / 4];
		}
		if (offset == 0x320) {
			return epss.wanted[group];
		}
	}
	epss.elsewhere++;
	fprintf(stderr, "MODEL: a read at %#lx, where nothing of the frequency block is\n", (unsigned long)address);
	return 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	unsigned group = epss.size ? (unsigned)((address - epss.base) / epss.size) : GROUPS;

	if (address >= epss.base && group < GROUPS && (address - epss.base) % epss.size == 0x320) {
		epss.wanted[group] = value;
		epss.writes[group]++;
		return;
	}
	epss.elsewhere++;
	fprintf(stderr, "MODEL: a write of %#x at %#lx, which is not a group's row register\n", value, (unsigned long)address);
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the driver's neighbours, which its start calls (bsd/dev/hakios_ddr.c, hakios_bench.c) */
static unsigned load_meters, memory_votes, benchmarks;
static bool vote_with_cpufreq;
void hakios_cpuload_start(void) { load_meters++; }
void hakios_ddr_init(bool with_cpufreq) { memory_votes++; vote_with_cpufreq = with_cpufreq; }
void hakios_membench(void) { benchmarks++; }
void hakios_cpufreq_init(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "hakios_board.c"
#include "hakios_cpufreq.c"

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
	if (b != NULL) {
		epss.base = b->cpufreq.epss;
		epss.size = b->cpufreq.epss_domain_size;
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
	boot_args = "cpufreq=1";
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
	boot_args = "cpufreq=1";
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
	boot_args = "cpufreq=1";
	hakios_cpufreq_init();
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8 && epss.wanted[2] == 1, "rows %u %u %u", epss.wanted[0], epss.wanted[1],
	    epss.wanted[2]);
	CHECK(logged("group 1 (1 of our cores)"), "the big group with the one core the kernel has of it");
}

/* a chip with two groups, six and two cores, its block elsewhere and its groups further apart */
static void
another_soc(void)
{
	phone("phone-keys", 8);
	boot_args = "cpufreq=1";
	hakios_cpufreq_init();
	CHECK(epss.base == 0x05591000 && epss.size == 0x2000, "the block is where that SoC file has it: %#x, %#x", epss.base, epss.size);
	CHECK(epss.wanted[0] == 7 && epss.wanted[1] == 8, "rows %u %u", epss.wanted[0], epss.wanted[1]);
	CHECK(epss.writes[2] + epss.writes[3] == 0, "it has no third group");
	CHECK(logged("group 0 (6 of our cores)") && logged("group 1 (2 of our cores)"), "six and two: %s", log_text);
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
	boot_args = "cpufreq=1";
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
	epss.base = 0x18591000;
	epss.size = 0x1000;
	boot_args = "cpufreq=1";
	hakios_cpufreq_init();
	CHECK(logged("cpufreq: disabled: this board's board.toml lists no \"cpufreq-epss\""), "said: %s", log_text);
	CHECK(all_writes() == 0 && epss.elsewhere == 0, "nothing read or written");
}

/* a machine nobody described */
static void
unknown_board(void)
{
	phone("nobody-described-this", 8);
	boot_args = "cpufreq=1";
	hakios_cpufreq_init();
	CHECK(logged("cpufreq: disabled"), "said: %s", log_text);
	CHECK(all_writes() == 0 && epss.elsewhere == 0, "nothing read or written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "every_group", every_group }, { "one_core", one_core }, { "some_cores", some_cores }, { "another_soc", another_soc },
		{ "odd_tables", odd_tables }, { "off_by_default", off_by_default }, { "not_listed", not_listed },
		{ "unknown_board", unknown_board },
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
