/*
 * The clock speed of the processor cores the kernel runs on: the feature "cpufreq-epss".
 *
 *   cpufreq=1      every group of cores that has a core of ours is asked for the top row of its table
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

#include "hakios_board.h"
#include "hakios_log.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* (the loader's log as well: this runs early, and the kernel's own log of a run keeps only its end) */
#define tell hakios_say

/*
 * Where the EPSS block is and which cores each of its groups ("domains") has is the board's data
 * (hakios_board.h; soc/SOC.toml [cpufreq]): on the two phones this was written for, three groups of
 * 0x1000 at 0x18591000 -- four small cores, three big, one biggest.
 */
#define EPSS_DOMAINS            HAKIOS_CPU_GROUPS
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/*
 * The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second
 * field), by the board's data; -1 for a core the board's groups do not reach.
 */
static int
domain_of(uint32_t phys_id)
{
	return hakios_board_cpu_group_of((phys_id >> 8) & 0xff);
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(hakios_board()->cpufreq.epss + domain * hakios_board()->cpufreq.epss_domain_size,
	    hakios_board()->cpufreq.epss_domain_size);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
	}
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

void
redwood_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	hakios_cpuload_start();
	PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled));
	hakios_ddr_init(enabled != 0);          /* (the memory first, as before: bsd/dev/hakios_ddr.c) */
	if (!enabled) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS)) {
		printf("cpufreq: disabled: this board's board.toml lists no \"cpufreq-epss\"\n");
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		int domain = domain_of(topology->cpus[i].phys_id);

		if (domain >= 0) {
			cores[domain]++;
		}
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	hakios_membench();                      /* (after the cores' clocks: bsd/dev/hakios_bench.c) */
}
