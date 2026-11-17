/*
 * The clock speed of the processor cores the kernel runs on: the feature "cpufreq-epss", on every Snapdragon since
 * the SDM845.
 *
 *   cpufreq=1      every group of cores that has a core of ours is asked for the top row of its table
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. That is one register write: the hardware (EPSS on the SM8350 and every
 * chip after it, the OSM's firmware on the SDM845 to the SM8250) has a table of frequencies per
 * group and switches the clock source and the supply voltage by itself when told which row is
 * wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layouts: board/cpufreq_hw.h (Linux drivers/cpufreq/qcom-cpufreq-hw.c, both of its variants). A group
 * whose hardware is not in charge of its clock (the block's enable bit clear) is left alone, as Linux leaves it.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */

#include "kernel.h"
#include "board.h"
#include "cpufreq_hw.h"
#include "kconfig.h"
#include "log.h"
#include "mmio.h"
#include "time.h"

extern void hakios_tl(const char *format, ...) __printflike(1, 2);   /* the kernel's log beside the console (input.c) */

/* (the loader's log as well: this runs early, and the kernel's own log of a run keeps only its end) */
#define tell hakios_say

/*
 * Where the block is, which layout it has and which cores each of its groups ("domains") has is the board's
 * data (board/board.h; soc.dtsi, cpufreq { }): on the two phones this was written for, three EPSS groups of
 * 0x1000 at 0x18591000 -- four small cores, three big, one biggest.
 */
#define CPUFREQ_GROUPS          HAKIOS_CPU_GROUPS

static const struct cpufreq_hw_layout *layout = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_EPSS];  /* (the board's, at the start) */

/* a group's block mapped, or 0 when the hardware is not in charge of the group's clock (Linux refuses such a group too) */
static vm_offset_t
domain_map(uint32_t domain)
{
	const struct hakios_board *board = hakios_board();
	vm_offset_t base = ml_io_map(cpufreq_hw_domain(board, domain), cpufreq_hw_map_size(board));

	return (rd(base + layout->enable) & 1) ? base : 0;
}

/*
 * the top row of a group's table (it ends by repeating its last row), and its frequency; and, for the log, each row's core
 * count as one digit (CPUFREQ_HW_CORE_COUNT: rows of count 1 are the ones Linux keeps out as turbo), "4441" for four rows
 */
static uint32_t
table_top(vm_offset_t base, uint32_t *top_khz, char counts[CPUFREQ_HW_ROWS + 1])
{
	uint32_t top = 0;

	*top_khz = 0;
	counts[0] = 0;
	for (uint32_t row = 0; row < CPUFREQ_HW_ROWS; row++) {
		uint32_t entry = rd(base + cpufreq_hw_row(layout, row));
		uint32_t khz = cpufreq_hw_khz(entry);

		if (row > 0 && khz <= *top_khz) {
			break;
		}
		top = row;
		*top_khz = khz;
		counts[row] = (char)('0' + CPUFREQ_HW_CORE_COUNT(entry));
		counts[row + 1] = 0;
	}
	return top;
}

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
	char counts[CPUFREQ_HW_ROWS + 1];
	bool turbo = false;
	vm_offset_t base;

	base = domain_map(domain);
	if (base == 0) {
		tell("cpufreq: group %u (%u of our cores): the hardware is not in charge of its clock: left alone\n", domain, cores);
		return;
	}
	top = table_top(base, &top_khz, counts);
	was = rd(base + layout->perf_state);
	wr(base + layout->perf_state, top);
	sleep_ms(20);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
	/* (only looked at: whether the top rows are ones Linux would leave out, which a phone's log is to tell) */
	for (unsigned i = 0; counts[i] != 0; i++) {
		turbo |= counts[i] == '0' + CPUFREQ_HW_TURBO;
	}
	tell("cpufreq: group %u: the rows' core counts %s%s\n", domain, counts, turbo ? " (1: a row Linux leaves out as turbo)" : "");
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
	const struct cpufreq_hw_layout *l3 = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_EPSS];  /* (EPSS's, on either kind of chip) */
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
	for (uint32_t row = 0; row < CPUFREQ_HW_ROWS; row++) {
		uint32_t khz = cpufreq_hw_khz(*(volatile uint32_t *)(base + cpufreq_hw_row(l3, row)));

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
	was = *(volatile uint32_t *)(base + l3->perf_state);
	if (enabled != 0) {
		*(volatile uint32_t *)(base + l3->perf_state) = top;
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
 * The system cache (LLCC: between the cores' L3 and the memory, shared with the GPU and the rest; Linux
 * drivers/soc/qcom/llcc-qcom.c with sc7280's table, the device tree's "qcom,sc7280-llcc" at 0x9200000, 0x9280000
 * and the broadcast block at 0x9600000). Nothing here programmed it: whether the processors' slice is in use is
 * whatever the boot loader left. Boot-arg llcc=1: a look (the block's version, its banks, each slice's attributes
 * and state); llcc=2: the processors' slice (1) and the one of the SMMU's table walks (13) programmed as Linux does
 * and activated, the GPU's two (11, 12) programmed only (activating those is the GPU's own business), and a load
 * chasing through 1-6 MB before and after. The slices activated here are taken back on the way to the loader.
 * What it found here (logs/194716-run.log in hakios-sm7325): version 2.0, two banks, no slice programmed or active.
 * With the processors' slice on, the kernel's chase through 3 MB took 20.7 ns a load against 25.8 (1-2 MB and 4-6 MB
 * the same), but nothing else measured moved: SpringBoard's turn after a tap on an icon 180-190 ms against 196-197,
 * the gap before the opening animation 225-233 ms against 192-242. So it is left off, as the L3's clock is.
 */
#define LLCC_BANK0              0x09200000
#define LLCC_BCAST              0x09600000
#define LLCC_SIZE               0x58000
#define LLCC_HW_INFO            0x30000         /* (broadcast) the version: 0xMMmmpp00 */
#define LLCC_STATUS0            0x3000c         /* (a bank) [31:28] the number of banks */
#define LLCC_ACT_CTRL(n)        ((n) * 0x1000)  /* [0] trigger, [2:1] opcode: 1 activate, 2 deactivate */
#define LLCC_ACT_STATUS(n)      (4 + (n) * 0x1000)      /* [0] active, [1] inactive */
#define LLCC_ATTR0(n)           (0x21000 + 8 * (n))     /* [15:0] reserved ways, [31:16] bonus ways */
#define LLCC_ATTR1(n)           (0x21004 + 8 * (n))     /* [1:0] mode, [2] probe target, [3] fixed, [6:4] priority, [31:16] lines a bank */
#define LLCC_DIS_CAP_ALLOC      0x21f00
#define LLCC_PCB_ACT            0x21f04         /* a bit a slice: kept through a power collapse */
#define LLCC_WRSC_EN            0x21f20
#define LLCC_WRSC_CACHEABLE_EN  0x21f2c
#define LLCC_CHASE_MAX          (6u << 20)

static const struct llcc_slice {
	uint8_t id, priority, fixed, retain, activate;
	uint16_t kb;
} llcc_slices[] = {
	{ 1, 1, 0, 1, 1, 768 },         /* CPUSS: the processors */
	{ 11, 1, 1, 1, 0, 256 },        /* GPUHTW: the GPU's SMMU table walks */
	{ 12, 1, 0, 1, 0, 512 },        /* GPU */
	{ 13, 1, 1, 0, 1, 256 },        /* MMUHWT: the system SMMU's table walks */
};
static vm_offset_t llcc_bcast;
static bool llcc_activated;

static void
llcc_chain(vm_offset_t buffer, uint32_t *perm, uint32_t bytes)
{
	uint32_t lines = bytes / 64, seed = 12345;

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
}

static void
llcc_chase_all(vm_offset_t buffer, uint32_t *perm, const char *when)
{
	static const uint32_t sizes[] = { 1u << 20, 2u << 20, 3u << 20, 4u << 20, 6u << 20 };
	char line[200];
	int used = 0;

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		llcc_chain(buffer, perm, sizes[i]);
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %u KB %llu/100 ns,", sizes[i] >> 10,
		    l3_chase_ns(buffer));
	}
	tell("llcc: a load chasing through%s %s\n", line, when);
}

static bool
llcc_act(uint32_t id, bool activate)
{
	volatile uint32_t *ctrl = (volatile uint32_t *)(llcc_bcast + LLCC_ACT_CTRL(id));
	volatile uint32_t *status = (volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(id));
	uint32_t opcode = activate ? 1u << 1 : 2u << 1, waiting_for = activate ? 2u : 1u;

	*ctrl = opcode | 1;             /* the trigger up, then down (Linux: llcc_update_act_ctrl()) */
	*ctrl = opcode;
	for (int i = 0; i < 2000; i++) {        /* (reads only, no delay: this runs with interrupts off as well) */
		if ((*status & waiting_for) == 0) {
			return true;
		}
	}
	return false;
}

static void
llcc_setup(void)
{
	uint32_t mode = 0, version, banks;
	vm_offset_t bank0, buffer = 0;
	uint32_t *perm = NULL;

	PE_parse_boot_argn("llcc", &mode, sizeof(mode));
	if (mode == 0) {
		return;
	}
	/* (an SM8250's OSM is where an SM7325's EPSS is, and its slices are not these) */
	if (hakios_board()->cpufreq.epss != L3_EPSS + 0x1000 || hakios_board()->cpufreq.block != HAKIOS_CPUFREQ_EPSS) {
		tell("llcc: not this SoC (the cores' %s at %#x): left alone\n", layout->name, hakios_board()->cpufreq.epss);
		return;
	}
	llcc_bcast = ml_io_map(LLCC_BCAST, LLCC_SIZE);
	bank0 = ml_io_map(LLCC_BANK0, LLCC_SIZE);
	version = *(volatile uint32_t *)(llcc_bcast + LLCC_HW_INFO);
	banks = *(volatile uint32_t *)(bank0 + LLCC_STATUS0) >> 28;
	tell("llcc: version %#x, %u banks; kept through power collapse %#x, writes' slices %#x/%#x, no capacity %#x\n",
	    version, banks, *(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT), *(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_EN),
	    *(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_CACHEABLE_EN), *(volatile uint32_t *)(llcc_bcast + LLCC_DIS_CAP_ALLOC));
	for (uint32_t id = 0; id < 32; id++) {
		uint32_t a0 = *(volatile uint32_t *)(llcc_bcast + LLCC_ATTR0(id)), a1 = *(volatile uint32_t *)(llcc_bcast + LLCC_ATTR1(id));
		uint32_t st = *(volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(id));

		if (a0 != 0 || a1 != 0 || (st & 1) != 0) {
			tell("llcc: slice %2u: %s, ways %#x+%#x, priority %u%s, %u KB\n", id, st & 1 ? "ACTIVE" : st & 2 ? "inactive" : "state 0",
			    a0 & 0xffff, a0 >> 16, (a1 >> 4) & 7, a1 & 8 ? " fixed" : "", (a1 >> 16) * 64 * (banks ? banks : 1) / 1024);
		}
	}
	if (mode != 2) {
		return;
	}
	if (version >= 0x04010000 || banks == 0 || banks > 4) {
		tell("llcc: not the block this was written for: left alone\n");
		return;
	}
	if (kmem_alloc(kernel_map, &buffer, LLCC_CHASE_MAX, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, (vm_offset_t *)&perm, LLCC_CHASE_MAX / 64 * sizeof(uint32_t), KMA_KOBJECT | KMA_DATA,
	    VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		if (buffer != 0) {
			kmem_free(kernel_map, buffer, LLCC_CHASE_MAX);
		}
		buffer = 0;
	}
	if (buffer != 0) {
		llcc_chase_all(buffer, perm, "before");
	}
	for (unsigned i = 0; i < sizeof(llcc_slices) / sizeof(llcc_slices[0]); i++) {
		const struct llcc_slice *s = &llcc_slices[i];
		uint32_t lines = s->kb * 1024u / banks / 64, bit = 1u << s->id;

		*(volatile uint32_t *)(llcc_bcast + LLCC_ATTR1(s->id)) = (uint32_t)s->fixed << 3 | (uint32_t)s->priority << 4 | lines << 16;
		*(volatile uint32_t *)(llcc_bcast + LLCC_ATTR0(s->id)) = 0x3fu << 16;
		*(volatile uint32_t *)(llcc_bcast + LLCC_DIS_CAP_ALLOC) &= ~bit;
		*(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT) = (*(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT) & ~bit) |
		    (s->retain ? bit : 0);
		if (version >= 0x02000000) {
			*(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_EN) &= ~bit;
		}
		if (version >= 0x02010000) {
			*(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_CACHEABLE_EN) &= ~bit;
		}
		if (s->activate) {
			bool done = llcc_act(s->id, true);

			llcc_activated = llcc_activated || done;
			tell("llcc: slice %u (%u KB) activated: %s, state %#x\n", s->id, s->kb, done ? "yes" : "NO",
			    *(volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(s->id)));
		}
	}
	if (buffer != 0) {
		llcc_chase_all(buffer, perm, "after");
		kmem_free(kernel_map, buffer, LLCC_CHASE_MAX);
		kmem_free(kernel_map, (vm_offset_t)perm, LLCC_CHASE_MAX / 64 * sizeof(uint32_t));
	}
}

/* On the way back to the loader (interrupts off): the slices activated above, deactivated. */
static void
llcc_quiesce(void)
{
	if (!llcc_activated) {
		return;
	}
	llcc_activated = false;
	for (unsigned i = 0; i < sizeof(llcc_slices) / sizeof(llcc_slices[0]); i++) {
		if (llcc_slices[i].activate) {
			(void)llcc_act(llcc_slices[i].id, false);
		}
	}
	tell("llcc: the slices activated at boot deactivated for the loader\n");
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
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);    /* osfmk/arm64/machine_routines.c (patches/0002) */

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
} gov[CPUFREQ_GROUPS];
static bool gov_on;

static void
gov_set(struct gov_group *g, uint32_t row)
{
	if (row != g->row) {
		wr(g->base + layout->perf_state, row);
		g->row = row;
	}
}

/* the groups the kernel has every core of: their tables, and the governor on (cpufreq=1 has raised them) */
static void
gov_start(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	const struct hakios_board *board = hakios_board();
	uint32_t on = 1, thermal = 1, had[CPUFREQ_GROUPS] = { 0 };

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
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t top_khz = 0;

		if (had[d] == 0 || had[d] != board->cpufreq.domain_cores[d]) {
			continue;               /* (none of ours, or a core of the loader's in it) */
		}
		g->base = domain_map(d);
		if (g->base == 0) {
			continue;               /* (not the hardware's: raise_domain() has said so) */
		}
		for (uint32_t row = 0; row < GOV_ROWS && row < CPUFREQ_HW_ROWS; row++) {
			uint32_t khz = cpufreq_hw_khz(rd(g->base + cpufreq_hw_row(layout, row)));

			if (row > 0 && khz <= top_khz) {
				break;
			}
			g->khz[row] = top_khz = khz;
			g->rows = row + 1;
		}
		g->top = g->rows - 1;
		/* (an index into khz[] and looks_at[]: never past the table, whatever the register reads; Linux bounds it too) */
		g->row = MIN(rd(g->base + layout->perf_state), g->top);
		gov_on = true;
		printf("cpufreq: group %u (cores %#x) by its load: %u rows, %u to %u MHz\n", d, g->cores, g->rows, g->khz[0] / 1000,
		    g->khz[g->top] / 1000);
	}
}

/* a look: BUSY the share of the last look each core was busy, in percent */
static void
gov_look(const uint32_t *busy)
{
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
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
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		if (gov[d].base != 0) {
			wr(gov[d].base + layout->perf_state, gov[d].top);
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
			for (uint32_t d = 0; gov_on && d < CPUFREQ_GROUPS && at < sizeof(rows) - 16; d++) {
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

/* every group back at its top row and the cache slices as the boot loader left them, for the loader (hakios_ddr_quiesce) */
void
hakios_cpufreq_quiesce(void)
{
	if (gov_on) {
		gov_quiesce();
	}
	llcc_quiesce();
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[CPUFREQ_GROUPS] = { 0 }, before, here;

	hakios_cpuload_start();
	PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled));
	if (CONFIG_QCOM_DDR_VOTE) {
		hakios_ddr_init(enabled != 0);  /* (the memory first, as before: bsd/dev/board/ddr.c) */
	}
	if (!enabled) {
		park_start();           /* (the small cores parked all the same: no clocks are touched for that) */
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS)) {
		tell("cpufreq: disabled: this board's board.dts lists no \"cpufreq-epss\"\n");
		park_start();
		return;
	}
	layout = cpufreq_hw_layout(hakios_board());
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		int domain = domain_of(topology->cpus[i].phys_id);

		if (domain >= 0) {
			cores[domain]++;
		}
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < CPUFREQ_GROUPS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	tell("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	hakios_membench();                      /* (after the cores' clocks: bsd/dev/board/bench.c) */
	l3_raise();
	llcc_setup();
	/* the groups by their load from here on (cpu_gov), and the small cores parked (park_small): one thread looks */
	gov_start();
	park_start();
}
