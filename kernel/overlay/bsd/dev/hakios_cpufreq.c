/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
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
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */

#include "hakios_board.h"
#include "hakios_log.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
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

/*
 * The L3 cache's clock, for a look (boot-arg l3=1: raised to its top row; l3_probe=1: its table, the block's words that
 * are not 0, and a load chasing through 2 MB -- past L2, inside L3 -- before and after). Its EPSS block, the device
 * tree's epss_l3_cpu ("qcom,lahaina-epss-l3-cpu", reg 0x18590000 0x4000), sits just before the cores' groups and has
 * their layout: the table at +0x100 (300-1516 MHz here), the row wanted at +0x320 (upstream Linux's sc7280/sm8250
 * epss-l3), which the boot loader leaves at row 0. Raising it changed nothing measured: the chase 7.28 ns against 7.21
 * after, SpringBoard's turn after a tap on an icon 188-193 ms against 190-230 (logs/182753-run.log in hakios-sm7325;
 * a first look, 8.11 against 6.83, did not come again). So it is left where the boot loader put it.
 */
#define L3_EPSS                 0x18590000
#define L3_CHASE_BYTES          (2u << 20)
#define L3_CHASE_STEPS          (1u << 20)

static uint64_t
l3_chase_ns(vm_offset_t buffer)
{
	uint64_t began, t = 0;
	volatile uint64_t *at = (volatile uint64_t *)buffer;

	for (uint32_t i = 0; i < L3_CHASE_STEPS / 8; i++) {
		at = (volatile uint64_t *)(uintptr_t)*at;       /* (warm) */
	}
	began = mach_absolute_time();
	for (uint32_t i = 0; i < L3_CHASE_STEPS; i++) {
		at = (volatile uint64_t *)(uintptr_t)*at;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
	(void)at;
	return t * 100 / L3_CHASE_STEPS;        /* hundredths of a ns a load */
}

static void
l3_raise(void)
{
	uint32_t enabled = 0, probe = 0, top = 0, top_khz = 0, was, lines = L3_CHASE_BYTES / 64, seed = 12345;
	vm_offset_t base, buffer = 0;
	char line[600];
	int used = 0;

	PE_parse_boot_argn("l3", &enabled, sizeof(enabled));
	PE_parse_boot_argn("l3_probe", &probe, sizeof(probe));
	if (enabled == 0 && probe == 0) {
		return;
	}
	if (hakios_board()->cpufreq.epss != L3_EPSS + 0x1000) {
		tell("l3: not this SoC's layout (the cores' EPSS at %#x): left alone\n", hakios_board()->cpufreq.epss);
		return;
	}
	base = ml_io_map(L3_EPSS, 0x1000);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;
		}
		top = row;
		top_khz = khz;
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %u:%u", row, khz / 1000);
	}
	if (probe) {
		tell("l3: frequency table (row:MHz)%s\n", line);
		used = 0;
		for (uint32_t off = 0; off < 0x400 && used < (int)sizeof(line) - 24; off += 4) {
			uint32_t w;

			if (off >= 0x100 && off < 0x300) {
				continue;
			}
			w = *(volatile uint32_t *)(base + off);
			if (w != 0) {
				used += snprintf(line + used, sizeof(line) - (size_t)used, " %x=%x", off, w);
			}
		}
		tell("l3: words not 0:%s\n", line);
		if (kmem_alloc(kernel_map, &buffer, L3_CHASE_BYTES, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) == KERN_SUCCESS) {
			/* a random cycle through every 64-byte line: each load waits for the one before */
			static uint32_t perm[L3_CHASE_BYTES / 64];

			for (uint32_t i = 0; i < lines; i++) perm[i] = i;
			for (uint32_t i = lines - 1; i > 0; i--) {
				uint32_t j, t;

				seed = seed * 1103515245u + 12345u;
				j = (seed >> 8) % (i + 1);
				t = perm[i]; perm[i] = perm[j]; perm[j] = t;
			}
			for (uint32_t i = 0; i < lines; i++) {
				*(uint64_t *)(buffer + 64ull * perm[i]) = buffer + 64ull * perm[(i + 1) % lines];
			}
			tell("l3: a load through 2 MB: %llu/100 ns\n", l3_chase_ns(buffer));
		}
	}
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	if (enabled != 0) {
		*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
		delay_for_interval(20, NSEC_PER_MSEC);
		tell("l3: row %u -> %u of its frequency table, %u MHz\n", was, top, top_khz / 1000);
	}
	if (buffer != 0) {
		if (enabled != 0) {
			tell("l3: a load through 2 MB now: %llu/100 ns\n", l3_chase_ns(buffer));
		}
		kmem_free(kernel_map, buffer, L3_CHASE_BYTES);
	}
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		hakios_tl("%s\n", line);
	}
}

/*
 * The small cores parked while the big ones have room (boot-arg park_small=0: not). The scheduler of this kernel
 * (Clutch, on a configuration without SMT) treats every core alike, so the window server's and SpringBoard's threads
 * landed on the Cortex-A55s as often as anywhere, some two and a half times slower than on a Cortex-A78 -- the 16.5
 * kernel made each small core the SMT sibling of a big one, which Clutch refuses ("The clutch scheduler does not
 * support CONFIG_SCHED_SMT"). Measured on the phone (rc.jobs.fps, the 0.7 s after each gesture): with the three small
 * cores, 47.8 frames a second on the home screen's pages and 19 gaps of 50 ms or more; with cpus=4, the big ones alone,
 * 51.5 and 6 (logs/110921-run.log, logs/112454-run.log). The small cores are not taken away, though: they are left out
 * of the scheduler's recommended cores (as Apple's performance controller does with cores it wants idle) while the big
 * ones have time to spare, and recommended again as soon as every big core was busy for most of PARK_MS -- a build or
 * a burst of compiles gets all seven, a gesture the four. The scheduler's own failsafe gives every core back to a
 * real-time thread that would otherwise wait.
 */
#define PARK_MS                 8               /* how often the big cores' load is looked at */
#define PARK_BUSY               85              /* % busy of the least busy big core: every big core is taken */
#define PARK_CALM               60              /* % busy of the least busy big core under which the small ones go again */
#define PARK_CALM_TIMES         8               /* ... that many looks in a row (64 ms) */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);

static struct {
	uint32_t looks, unparked, parks;        /* since the last line in the log */
} park_stat;
static bool park_wanted;

/*
 * The speed of each group of cores by its load (boot-arg cpu_gov=0: every group at its top row all along, as
 * before). With cpufreq=1 every group was raised to its top row and kept there: idle cores too stood at the top
 * row's voltage, and the SoC ran at 81-86 C on rc.jobs.fps (its thermal lines, logs/140658-run.log), where its
 * own limits hardware held the big cores to 2131 of 2400 MHz and the prime to 2208. Now, at every look (PARK_MS),
 * each group the kernel has all of is set to the lowest row of its table that covers what its busiest core did
 * at the speed it had, with GOV_HEADROOM_PCT to spare: up at once, to the top at once from GOV_UP_BUSY, and down
 * only after GOV_DOWN_LOOKS looks wanted it, to the most any of them wanted. A group with a core the kernel has
 * not got -- the small one, whose core 0 is the loader's, serving the flash while the kernel runs -- stays at
 * its top row. Not with thermal=2, which holds the rows back itself. At the run's end every group is put back
 * at its top row (hakios_ddr_quiesce, on every way back).
 */
#define GOV_HEADROOM_PCT        125
#define GOV_UP_BUSY             90
#define GOV_DOWN_LOOKS          4
#define GOV_ROWS                40
static struct gov_group {
	vm_offset_t base;               /* 0: not governed */
	uint32_t rows, khz[GOV_ROWS], top, row, down_looks, down_most;
	uint32_t cores;                 /* the kernel's cores in it, by cpu number */
	uint64_t looks_at[GOV_ROWS];    /* (for the log: how many looks at each row) */
} gov[EPSS_DOMAINS];
static bool gov_on;

static void
gov_set(struct gov_group *g, uint32_t row)
{
	if (row != g->row) {
		*(volatile uint32_t *)(g->base + EPSS_PERF_STATE) = row;
		g->row = row;
	}
}

/* the groups the kernel has every core of: their tables, and the governor on (cpufreq=1 has raised them) */
static void
gov_start(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	const struct hakios_board *board = hakios_board();
	uint32_t on = 1, thermal = 1, had[EPSS_DOMAINS] = { 0 };

	PE_parse_boot_argn("cpu_gov", &on, sizeof(on));
	PE_parse_boot_argn("thermal", &thermal, sizeof(thermal));
	if (!on || thermal >= 2) {
		printf("cpufreq: every group at its top row (%s)\n", !on ? "cpu_gov=0" : "thermal=2 holds them back itself");
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus && i < 32; i++) {
		int d = domain_of(topology->cpus[i].phys_id);

		if (d >= 0) {
			gov[d].cores |= 1u << i;
			had[d]++;
		}
	}
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t top_khz = 0;

		if (had[d] == 0 || had[d] != board->cpufreq.domain_cores[d]) {
			continue;               /* (none of ours, or a core of the loader's in it) */
		}
		g->base = ml_io_map(board->cpufreq.epss + d * board->cpufreq.epss_domain_size, board->cpufreq.epss_domain_size);
		for (uint32_t row = 0; row < GOV_ROWS && row < EPSS_TABLE_ROWS; row++) {
			uint32_t entry = *(volatile uint32_t *)(g->base + EPSS_FREQ_TABLE + 4 * row);
			uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

			if (row > 0 && khz <= top_khz) {
				break;
			}
			g->khz[row] = top_khz = khz;
			g->rows = row + 1;
		}
		g->top = g->rows - 1;
		g->row = *(volatile uint32_t *)(g->base + EPSS_PERF_STATE);
		gov_on = true;
		printf("cpufreq: group %u (cores %#x) by its load: %u rows, %u to %u MHz\n", d, g->cores, g->rows, g->khz[0] / 1000,
		    g->khz[g->top] / 1000);
	}
}

/* a look: BUSY the share of the last look each core was busy, in percent */
static void
gov_look(const uint32_t *busy)
{
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t most = 0, want, need;

		if (g->base == 0) {
			continue;
		}
		for (unsigned int i = 0; i < 32; i++) {
			if ((g->cores >> i) & 1) {
				most = MAX(most, busy[i]);
			}
		}
		/* what the busiest core did, at the speed it had, with room to spare */
		need = (uint32_t)((uint64_t)g->khz[g->row] * most * GOV_HEADROOM_PCT / 10000);
		for (want = 0; want < g->top && g->khz[want] < need; want++) {
		}
		if (most >= GOV_UP_BUSY) {
			want = g->top;
		}
		if (want >= g->row) {
			gov_set(g, want);
			g->down_looks = 0;
			g->down_most = 0;
		} else {
			g->down_most = MAX(g->down_most, want);
			if (++g->down_looks >= GOV_DOWN_LOOKS) {
				gov_set(g, g->down_most);
				g->down_looks = 0;
				g->down_most = 0;
			}
		}
		g->looks_at[g->row]++;
	}
}

/* (every way back to the loader, interrupts off: each governed group at its top row again) */
static void
gov_quiesce(void)
{
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		if (gov[d].base != 0) {
			*(volatile uint32_t *)(gov[d].base + EPSS_PERF_STATE) = gov[d].top;
		}
	}
	gov_on = false;
}

static void
park_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time(), said = then, ten;
	uint32_t big = 0, all = 0, calm = 0;
	bool parked;

	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS && i < 32; i++) {
		all |= 1u << i;
		if (topology->cpus[i].l2_access_penalty == 0) {
			big |= 1u << i;
		}
	}
	const bool parking = park_wanted && big != all && big != 0;

	if (!parking && !gov_on) {
		return;
	}
	nanoseconds_to_absolutetime(10 * NSEC_PER_SEC, &ten);
	parked = parking;
	if (parking) {
		sched_perfcontrol_update_recommended_cores(big);
		park_stat.parks++;
		printf("cpufreq: small cores %#x parked while the big ones (%#x) have room (park_small=0: not)\n", all & ~big, big);
	}
	for (;;) {
		uint64_t now, least = 100;
		uint32_t busy_of[32] = { 0 };

		delay_for_interval(PARK_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS && i < 32; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			busy_of[i] = (uint32_t)MIN(100, 100 * busy / (now - then));
			if ((big >> i) & 1) {
				least = MIN(least, 100 * busy / (now - then));
			}
		}
		then = now;
		park_stat.looks++;
		if (gov_on) {
			gov_look(busy_of);
		}
		if (!parking) {
			/* (no parking: the governor alone) */
		} else if (parked && least >= PARK_BUSY) {
			sched_perfcontrol_update_recommended_cores(all);
			parked = false;
			calm = 0;
			park_stat.unparked++;
		} else if (!parked) {
			calm = least < PARK_CALM ? calm + 1 : 0;
			if (calm >= PARK_CALM_TIMES) {
				sched_perfcontrol_update_recommended_cores(big);
				parked = true;
				park_stat.parks++;
			}
		}
		if (now - said >= ten) {
			char rows[96] = "";
			size_t at = 0;

			/* (and how many looks each governed group spent at each row, its top last: "g1 0:12 7:30 ...") */
			for (uint32_t d = 0; gov_on && d < EPSS_DOMAINS && at < sizeof(rows) - 16; d++) {
				if (gov[d].base == 0) {
					continue;
				}
				at += (size_t)snprintf(rows + at, sizeof(rows) - at, " g%u", d);
				for (uint32_t r = 0; r < gov[d].rows && at < sizeof(rows) - 12; r++) {
					if (gov[d].looks_at[r] != 0) {
						at += (size_t)snprintf(rows + at, sizeof(rows) - at, " %u:%llu", r, gov[d].looks_at[r]);
					}
				}
				memset(gov[d].looks_at, 0, sizeof(gov[d].looks_at));
			}
			hakios_tl("tl: k %u looks, %u times the small cores taken, %u times parked again;%s\n", park_stat.looks,
			    park_stat.unparked, park_stat.parks, rows);
			memset(&park_stat, 0, sizeof(park_stat));
			said = now;
		}
	}
}

static void
park_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("park_small", &on, sizeof(on));
	park_wanted = on != 0;
	if ((park_wanted || gov_on) && kernel_thread_start(park_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)      /* where it is: the SoC file's [rpmh] */
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(hakios_board()->rpmh.cmd_db, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			if (entry[i].length >= sizeof(aux) && slave->data_offset + entry[i].offset + sizeof(aux) < CMD_DB_SIZE - 0x40) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_SIZE                0x10000UL       /* where it is: the SoC file's [rpmh] */
#define RSC_TCS(n, reg)         (hakios_board()->rpmh.tcs_offset + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(hakios_board()->rpmh.rsc, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

/* (bsd/dev/hakios_input.c: command slot set 0 is the touch driver's and Wi-Fi's too, on renoir) */
extern void hakios_rpmh_lock(void);
extern void hakios_rpmh_unlock(void);

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(hakios_board()->rpmh.cmd_db, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as bsd/dev/hakios_goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(hakios_board()->rpmh.rsc, RSC_SIZE);
	hakios_rpmh_lock();
	ddr_voted = ddr_vote(vote);
	hakios_rpmh_unlock();
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hakios_ddr_quiesce(void);

void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (gov_on) {
		gov_quiesce();
	}
	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	/*
	 * Where the command database and the application processor's RPMh sequencer ("DRV 2") are comes
	 * from the SoC file's [rpmh]. What is taken for granted beyond that, as on both phones so far: the
	 * sequencer's first slot set is an active one, and the BCMs are named MC0, SH0 and ACV.
	 */
	if (!hakios_board_has(HAKIOS_HAS_DDR_RPMH)) {
		if (enabled != 0) tell("ddr: no RPMh vote on this board (its board.toml lists no \"ddr-rpmh\")\n");
	} else if (hakios_board()->rpmh.cmd_db == 0 || hakios_board()->rpmh.rsc == 0) {
		tell("ddr: no RPMh vote: the SoC file has no [rpmh]\n");
	} else if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		park_start();           /* (the small cores parked all the same: no clocks are touched for that) */
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS)) {
		printf("cpufreq: disabled: this board's board.toml lists no \"cpufreq-epss\"\n");
		park_start();
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
	l3_raise();
	/* the groups by their load from here on (cpu_gov), and the small cores parked (park_small): one thread looks */
	gov_start();
	park_start();
	if (PE_parse_boot_argn("membench", &enabled, sizeof(enabled)) && enabled == 1) {
		memory_bench();
	} else if (enabled == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
