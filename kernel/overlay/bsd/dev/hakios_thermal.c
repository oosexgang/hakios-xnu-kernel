/*
 * Keeping the phone from getting hot, for any device of the Qualcomm kind: one driver; what differs
 * between devices is the board's data (hakios_board.h: thermal { } of devices/NAME/board.dts, thermal { } and
 * cpufreq { } of its SoC's file), and the arithmetic is hakios_thermal_core.h (tested on the host:
 * tests/thermal_test.c, and the driver itself: tests/thermal_driver_test.c).
 *
 * Every second the SoC's temperature sensors are read (every TSENS controller, every sensor that has a
 * reading: Linux's tsens-v2.c; read only, never written), and from the hottest of them:
 *
 *   - the processors: each group's (EPSS domain's) frequency row is held back one row a second while the
 *     hottest is at or above th_trip, given back one row a second at or below th_clear, all the way down
 *     to the group's floor at once at th_critical. Never above the row the group was at when this started
 *     (cpufreq=1 raises them first; without it nothing is ever raised here), never below its floor (the
 *     first row at the profile's floor_mhz: the interface must stay usable);
 *   - the panel's light: hakios_backlight_set_cap(), from full when nothing is held back down to 60%
 *     when everything is (an AMOLED at full white is heat as well), as an iPhone dims when hot.
 *
 * At the run's end the rows are given back, so the loader is not left slow.
 *
 * Boot-args:
 *   thermal=0          off (default): nothing read
 *   thermal=1          the hottest reading logged every ten seconds, nothing held back
 *   thermal=2          held back as above
 *   th_trip=N th_clear=N th_critical=N   in tenths of a degree (default 700, 620, 950)
 *   th_period_ms=N     how often it looks (default 1000)
 *
 * sysctl hw.thermal: hottest (tenths of a degree), step and most (how far held back, of how far it can
 * be), trip, clear, critical (root may change them while it runs: tuning without a new kernel).
 *
 * A SoC without EPSS whose firmware has SCMI performance domains (SM8845: scmi_perf=2) is held back the
 * same way, a domain's levels standing for the rows, from the highest the firmware allows down to the
 * level the domain was found at.
 *
 * Adding a device: the TSENS controllers ("tsens_tm_physical") and the EPSS block ("cpufreq-hw") in its
 * SoC's file, the floors in its board file. GPU frequency is not touched here: the GPU's speed belongs to
 * its GMU (bsd/dev/hakios_adreno.c, boot-arg adreno_speed); hakios_thermal_step() tells it how far to go.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios_kernel.h"
#include "hakios_board.h"
#include "hakios_thermal_core.h"
#include "hakios_log.h"

extern void hakios_backlight_set_cap(uint32_t level);
extern int hakios_scmi_perf_domain(unsigned domain, uint32_t *found, uint32_t *top);
extern int hakios_scmi_perf_set(unsigned domain, uint32_t level);

#define say hakios_say

/* ---- what differs between devices: the board's data (hakios_board.h) -------------------------------- */

#define TH_CONTROLLERS          HAKIOS_TSENS_MAX        /* a SoC has up to so many TSENS controllers ... */
#define TH_SENSORS              16              /* ... of up to 16 sensors each */
#define TH_DOMAINS              HAKIOS_CPU_GROUPS       /* the groups of cores, at most (the board says how many) */

#define TSENS_STATUS(sensor)    (0xa0 + 4 * (sensor))
#define EPSS_FREQ_TABLE         0x100
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000

static const struct hakios_board *this_board;      /* (hakios_board(), looked up once) */
static uint32_t mode;
static struct th_policy policy = { 700, 620, 950 };
static vm_offset_t tsens[TH_CONTROLLERS], epss[TH_DOMAINS];
static uint32_t top_row[TH_DOMAINS], floor_row[TH_DOMAINS], row_now[TH_DOMAINS];
static unsigned groups;                         /* how many groups of cores the board has */
static bool scmi;                               /* the groups are SCMI performance domains (no EPSS) */
static volatile int32_t hottest = TH_NONE;
static volatile uint32_t step, most;

#define HAKIOS_MMIO_SYNC
#include "hakios_mmio.h"

/* the rows of the board's groups, as text: "8 8 8" */
static const char *
rows_text(const uint32_t *rows, char *text, size_t size)
{
	size_t at = 0;

	text[0] = 0;
	for (unsigned d = 0; d < groups && at < size; d++) {
		at += (size_t)snprintf(text + at, size - at, "%s%u", d ? " " : "", rows[d]);
	}
	return text;
}

/* the hottest reading of every sensor of every controller */
static int32_t
read_hottest(void)
{
	int32_t readings[TH_CONTROLLERS * TH_SENSORS];
	unsigned n = 0;

	for (unsigned c = 0; c < TH_CONTROLLERS; c++) {
		for (unsigned s = 0; tsens[c] != 0 && s < TH_SENSORS; s++) {
			int32_t t = TH_NONE;

			/* a sensor caught between two readings has none for a moment: asked again */
			for (unsigned again = 0; again < 4 && t == TH_NONE; again++) {
				t = th_tsens_tenths(rd(tsens[c] + TSENS_STATUS(s)));
			}
			readings[n++] = t;
		}
	}
	return th_hottest(readings, n);
}

/* a group's frequency table: its top row and the first row at or above its floor */
static void
read_table(unsigned d)
{
	uint32_t top_khz = 0;

	floor_row[d] = UINT32_MAX;
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = rd(epss[d] + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top_khz = khz;
		if (floor_row[d] == UINT32_MAX && khz >= this_board->thermal.floor_mhz[d] * 1000) {
			floor_row[d] = row;
		}
	}
	/* never above where the group is now: this only holds back, it does not raise */
	top_row[d] = row_now[d] = rd(epss[d] + EPSS_PERF_STATE);
	if (floor_row[d] == UINT32_MAX || floor_row[d] > top_row[d]) {
		floor_row[d] = top_row[d];
	}
}

static void
apply(uint32_t s)
{
	for (unsigned d = 0; d < TH_DOMAINS; d++) {
		uint32_t row = th_row(top_row[d], floor_row[d], s);

		if (d < groups && scmi && row != row_now[d]) {
			if (hakios_scmi_perf_set(d, row) == 0) {
				row_now[d] = row;
			}
		} else if (epss[d] != 0 && row != row_now[d]) {
			wr(epss[d] + EPSS_PERF_STATE, row);
			row_now[d] = row;
		}
	}
	hakios_backlight_set_cap(th_backlight_cap(s, most, 600));
}

#define PERIOD_MS       1000
static uint32_t period_ms = PERIOD_MS;          /* boot-arg th_period_ms (100 to 10000) */

static int poke;

static void
th_thread(__unused void *argument, __unused wait_result_t result)
{
	uint32_t looks = 0, said_step = 0;

	for (;;) {
		int32_t t = read_hottest();
		uint32_t next;

		hottest = t;
		if (mode >= 2) {
			next = th_next_step(step, most, t, &policy);
			if (next != step) {
				step = next;
				apply(step);
			}
			if (step != said_step) {
				char rows[48];

				say("thermal: %d.%d C: held back %u of %u (rows %s)\n", t / 10, t % 10 < 0 ? -(t % 10) : t % 10,
				    step, most, rows_text(row_now, rows, sizeof(rows)));
				said_step = step;
			}
		}
		if (looks++ % 10 == 0) {
			say("thermal: hottest sensor %d.%d C%s\n", t / 10, t % 10 < 0 ? -(t % 10) : t % 10,
			    mode >= 2 ? "" : " (thermal=1: looking only)");
		}
		assert_wait_timeout(&poke, THREAD_UNINT, period_ms, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* how far the processors are held back now, 0 to `*of` (for the GPU's driver, should it want to follow) */
uint32_t
hakios_thermal_step(uint32_t *of)
{
	if (of != NULL) {
		*of = most;
	}
	return step;
}

/* ---- sysctl hw.thermal ---------------------------------------------------------------------------- */

SYSCTL_NODE(_hw, OID_AUTO, thermal, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "keeping the phone from getting hot");
SYSCTL_INT(_hw_thermal, OID_AUTO, hottest, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(int32_t *, &hottest), 0,
    "the hottest sensor, tenths of a degree");
SYSCTL_UINT(_hw_thermal, OID_AUTO, step, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &step), 0,
    "how far held back");
SYSCTL_UINT(_hw_thermal, OID_AUTO, most, CTLFLAG_RD | CTLFLAG_LOCKED, __DEVOLATILE(uint32_t *, &most), 0,
    "how far it can be held back");
SYSCTL_INT(_hw_thermal, OID_AUTO, trip, CTLFLAG_RW | CTLFLAG_LOCKED, &policy.trip, 0, "held back from, tenths of a degree");
SYSCTL_INT(_hw_thermal, OID_AUTO, clear, CTLFLAG_RW | CTLFLAG_LOCKED, &policy.clear, 0, "given back at, tenths of a degree");
SYSCTL_INT(_hw_thermal, OID_AUTO, critical, CTLFLAG_RW | CTLFLAG_LOCKED, &policy.critical, 0,
    "all the way at once, tenths of a degree");

/* ---- start and end ------------------------------------------------------------------------------- */

void
hakios_thermal_init(void)
{
	thread_t thread;
	int32_t value;

	if (!PE_parse_boot_argn("thermal", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	this_board = hakios_board();
	if (this_board == NULL || this_board->thermal.tsens[0] == 0) {
		say("thermal: this board's file has no thermal { } (devices/NAME/board.dts): nothing read\n");
		mode = 0;
		return;
	}
	if (mode >= 2 && this_board->cpufreq.epss == 0) {
		uint32_t found, top;

		/* SCMI: the firmware's levels are the rows, its highest the top, where it was found the floor */
		for (groups = 0; groups < TH_DOMAINS && hakios_scmi_perf_domain(groups, &found, &top); groups++) {
			top_row[groups] = row_now[groups] = top;
			floor_row[groups] = found;
		}
		scmi = groups > 0;
		if (!scmi) {
			say("thermal: this board's SoC file has no cpufreq { } and SCMI gave no levels (scmi_perf=2): looking only\n");
			mode = 1;
		}
	}
	if (PE_parse_boot_argn("th_trip", &value, sizeof(value))) {
		policy.trip = value;
	}
	if (PE_parse_boot_argn("th_clear", &value, sizeof(value))) {
		policy.clear = value;
	}
	if (PE_parse_boot_argn("th_critical", &value, sizeof(value))) {
		policy.critical = value;
	}
	if (PE_parse_boot_argn("th_period_ms", &value, sizeof(value)) && value >= 100 && value <= 10000) {
		period_ms = (uint32_t)value;
	}
	if (policy.clear >= policy.trip || policy.trip >= policy.critical) {
		say("thermal: th_clear < th_trip < th_critical does not hold (%d, %d, %d): the defaults\n", policy.clear,
		    policy.trip, policy.critical);
		policy = (struct th_policy){ 700, 620, 950 };
	}
	for (unsigned c = 0; c < TH_CONTROLLERS; c++) {
		tsens[c] = this_board->thermal.tsens[c] ? ml_io_map(this_board->thermal.tsens[c], 0x200) : 0;
	}
	most = 0;
	if (scmi) {
		for (unsigned d = 0; d < groups; d++) {
			most = top_row[d] - floor_row[d] > most ? top_row[d] - floor_row[d] : most;
		}
	} else {
		groups = hakios_board_cpu_groups();
	}
	for (unsigned d = 0; d < groups && mode >= 2 && !scmi; d++) {
		epss[d] = ml_io_map(this_board->cpufreq.epss + d * this_board->cpufreq.epss_domain_size, this_board->cpufreq.epss_domain_size);
		read_table(d);
		most = top_row[d] - floor_row[d] > most ? top_row[d] - floor_row[d] : most;
	}
	{
		char tops[48], floors[48];

		say("thermal: %s: trip %d.%d C, clear %d.%d C, critical %d.%d C; the groups at rows %s, floors %s%s\n",
		    this_board->name, policy.trip / 10, policy.trip % 10, policy.clear / 10, policy.clear % 10,
		    policy.critical / 10, policy.critical % 10, rows_text(top_row, tops, sizeof(tops)),
		    rows_text(floor_row, floors, sizeof(floors)), mode >= 2 ? "" : " (thermal=1: looking only)");
	}
	if (kernel_thread_start(th_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "thermal");
		thread_deallocate(thread);
	}
}

/* At the run's end: the groups back where they were found. */
void
hakios_thermal_quiesce(void)
{
	/* SCMI: nothing is asked here -- the other processors are stopped and no lock may be taken; the
	 * loader runs at whatever level the firmware has, as it does after any SCMI request */
	if (mode < 2 || step == 0 || scmi) {
		return;
	}
	for (unsigned d = 0; d < TH_DOMAINS; d++) {
		if (epss[d] != 0 && row_now[d] != top_row[d]) {
			wr(epss[d] + EPSS_PERF_STATE, top_row[d]);
			row_now[d] = top_row[d];
		}
	}
	{
		char rows[48];

		say("thermal: at the run's end: the groups back at rows %s\n", rows_text(top_row, rows, sizeof(rows)));
	}
}
