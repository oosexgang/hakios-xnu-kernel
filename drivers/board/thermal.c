/*
 * Keeping the phone from getting hot, for any device of the Qualcomm kind: one driver; what differs
 * between devices is the board's data (board/board.h: thermal { } of arch/arm64/boot/dts/NAME/board.dts, thermal { } and
 * cpufreq { } of its SoC's file), and the arithmetic is board/thermal_core.h (tested on the host:
 * workbench/thermal_test.c, and the driver itself: workbench/thermal_driver_test.c).
 *
 * Every second the SoC's temperature sensors are read (every TSENS controller, every sensor that has a
 * reading: Linux's tsens-v2.c; read only, never written), and from the hottest of them:
 *
 *   - the processors: each group's (EPSS or OSM domain's: board/cpufreq_hw.h) frequency row is held back one row a second while the
 *     hottest is at or above th_trip, given back one row a second at or below th_clear, all the way down
 *     to the group's floor at once at th_critical. Never above the row the group was at when this started
 *     (cpufreq=1 raises them first; without it nothing is ever raised here), never below its floor (the
 *     first row at the profile's floor_mhz: the interface must stay usable);
 *   - the panel's light: hakios_backlight_set_cap(), from full when nothing is held back down to 60%
 *     when everything is (an AMOLED at full white is heat as well), as an iPhone dims when hot.
 *
 * At the run's end the rows are given back, so the loader is not left slow.
 *
 * And every look a line, "tl: h at US hottest TENTHS sensor N limit MHz,... asked MHz,... gpu MHz": the
 * hottest sensor; each group's frequency as the hardware allows it now (EPSS's DOMAIN_STATE, the OSM's current vote: the limit its
 * own LMh, the SoC's limits-management hardware, sets when hot -- below the row asked for when it throttles,
 * whatever this driver does) and as asked (PERF_STATE's row); and the GPU's core clock as the SoC's frequency
 * counter measures it (hakios_adreno_core_mhz(); 0 while the GPU is not up). A frame rate that drops as the
 * phone warms is then told apart from one that is slow by itself (darwin/tools/frames.py shows them per gesture).
 *
 * Boot-args:
 *   thermal=0          off: nothing read
 *   thermal=1          (the default) looking only: a "tl: h" line a look, the hottest reading logged every
 *                      ten seconds, nothing written
 *   thermal=2          held back as above
 *   th_trip=N th_clear=N th_critical=N   in tenths of a degree (default 700, 620, 950)
 *   th_period_ms=N     how often it looks (default 1000)
 *
 * sysctl hw.thermal: hottest (tenths of a degree), step and most (how far held back, of how far it can
 * be), trip, clear, critical (root may change them while it runs: tuning without a new kernel; each 0 to 125.0 C,
 * and while the three are out of order the last policy that was in order stays in force).
 *
 * A SoC without EPSS whose firmware has SCMI performance domains (SM8845: scmi_perf=2) is held back the
 * same way, a domain's levels standing for the rows, from the highest the firmware allows down to the
 * level the domain was found at.
 *
 * Adding a device: the TSENS controllers ("tsens_tm_physical") and the frequency block ("cpufreq-hw") in its
 * SoC's file, the floors in its board file. GPU frequency is not touched here: the GPU's speed belongs to
 * its GMU (bsd/dev/board/adreno.c, boot-arg adreno_speed); hakios_thermal_step() tells it how far to go.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "cpufreq_hw.h"
#include "thermal_core.h"
#include "log.h"

extern void hakios_backlight_set_cap(uint32_t level);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern uint32_t hakios_adreno_core_mhz(void);
extern int hakios_scmi_perf_domain(unsigned domain, uint32_t *found, uint32_t *top);
extern int hakios_scmi_perf_set(unsigned domain, uint32_t level);

#define say hakios_say

/* ---- what differs between devices: the board's data (board/board.h) -------------------------------- */

#define TH_CONTROLLERS          HAKIOS_TSENS_MAX        /* a SoC has up to so many TSENS controllers ... */
#define TH_SENSORS              16              /* ... of up to 16 sensors each */
#define TH_DOMAINS              HAKIOS_CPU_GROUPS       /* the groups of cores, at most (the board says how many) */

#define TSENS_STATUS(sensor)    (0xa0 + 4 * (sensor))
static const struct cpufreq_hw_layout *layout = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_EPSS];  /* (the board's, at the start) */

static const struct hakios_board *this_board;      /* (hakios_board(), looked up once) */
static uint32_t mode = 1;
static struct th_policy policy = { 700, 620, 950 };
/*
 * The policy the thread holds to: the last one that was in order (clear < trip < critical). root may tune the three
 * in any order -- a trip lowered before its clear is, for a moment, under it -- and meanwhile the last good one
 * stays in force rather than a muddle of both.
 */
static struct th_policy in_force = { 700, 620, 950 };
/* No threshold above this, from a boot-arg or from root: the critical step is what keeps a phone that runs away
 * from cooking itself, and a critical of 150 C would be none (silicon's junction limit is about 125 C). */
#define TH_THRESHOLD_MOST       1250
static vm_offset_t tsens[TH_CONTROLLERS], epss[TH_DOMAINS];
static uint32_t top_row[TH_DOMAINS], floor_row[TH_DOMAINS], row_now[TH_DOMAINS];
static unsigned groups;                         /* how many groups of cores the board has */
static bool scmi;                               /* the groups are SCMI performance domains (no EPSS) */
static volatile int32_t hottest = TH_NONE;
static volatile uint32_t step, most;

#define HAKIOS_MMIO_SYNC
#include "mmio.h"

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

/*
 * tenths of a degree as text, "72.0" or "-0.5": the sign said by itself, as the tenths alone would lose it between 0 and -1 C
 * (-5 tenths divided by ten is 0, and "0.5" is warm where -0.5 is a phone left out in the frost)
 */
static const char *
tenths_text(int32_t t, char *text, size_t size)
{
	snprintf(text, size, "%s%d.%d", t < 0 ? "-" : "", t / 10 < 0 ? -(t / 10) : t / 10, t % 10 < 0 ? -(t % 10) : t % 10);
	return text;
}

/*
 * The sensors that have had a reading at some look. One of them silent now (no reading in four tries) is not a sensor
 * that is not wired up: it is one whose temperature is not known this look -- and it may well be the hottest, the one
 * the phone was held back for. Its silence is no reason to give anything back (th_thread).
 */
static bool answered[TH_CONTROLLERS * TH_SENSORS];

/*
 * the hottest reading of every sensor of every controller, and which it is (controller * 16 + sensor); `*silent`: how many
 * of the sensors that answered at an earlier look have no reading now
 */
static int32_t
read_hottest(unsigned *which, unsigned *silent)
{
	int32_t readings[TH_CONTROLLERS * TH_SENSORS], hot;
	unsigned n = 0;

	*silent = 0;

	for (unsigned c = 0; c < TH_CONTROLLERS; c++) {
		for (unsigned s = 0; s < TH_SENSORS; s++) {
			int32_t t = TH_NONE;

			/* a sensor caught between two readings has none for a moment: asked again */
			for (unsigned again = 0; tsens[c] != 0 && again < 4 && t == TH_NONE; again++) {
				t = th_tsens_tenths(rd(tsens[c] + TSENS_STATUS(s)));
			}
			if (t != TH_NONE) {
				answered[n] = true;
			} else if (answered[n]) {
				(*silent)++;
			}
			readings[n++] = t;
		}
	}
	hot = th_hottest(readings, n);
	*which = 0;
	for (unsigned i = 0; i < n && hot != TH_NONE; i++) {
		if (readings[i] == hot) {
			*which = i;
			break;
		}
	}
	return hot;
}

/* a row's frequency in a group's table, in MHz */
static uint32_t
row_mhz(unsigned d, uint32_t row)
{
	return cpufreq_hw_khz(rd(epss[d] + cpufreq_hw_row(layout, row))) / 1000;
}

/* the line of this look: the hottest, each group's frequency allowed and asked, the GPU's */
static void
tell_look(int32_t t, unsigned which)
{
	char limits[48] = "", asked[48] = "";
	size_t al = 0, aa = 0;
	uint64_t now_ns;

	for (unsigned d = 0; d < groups && epss[d] != 0; d++) {
		al += (size_t)snprintf(limits + al, sizeof(limits) - al, "%s%u", d ? "," : "", CPUFREQ_HW_XO_KHZ * (rd(epss[d] + layout->limit) & layout->limit_mask) / 1000);
		aa += (size_t)snprintf(asked + aa, sizeof(asked) - aa, "%s%u", d ? "," : "", row_mhz(d, rd(epss[d] + layout->perf_state)));
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	hakios_tl("tl: h at %llu hottest %d sensor %u limit %s asked %s gpu %u\n", now_ns / 1000, t, which,
	    limits[0] ? limits : "-", asked[0] ? asked : "-", hakios_adreno_core_mhz());
}

/* a group's frequency table: its top row and the first row at or above its floor */
static void
read_table(unsigned d)
{
	uint32_t top_khz = 0, last = 0;

	floor_row[d] = UINT32_MAX;
	for (uint32_t row = 0; row < CPUFREQ_HW_ROWS; row++) {
		uint32_t khz = cpufreq_hw_khz(rd(epss[d] + cpufreq_hw_row(layout, row)));

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top_khz = khz;
		last = row;
		if (floor_row[d] == UINT32_MAX && khz >= this_board->thermal.floor_mhz[d] * 1000) {
			floor_row[d] = row;
		}
	}
	/*
	 * never above where the group is now: this only holds back, it does not raise -- nor past the table's last row,
	 * whatever the register reads (Linux's qcom_cpufreq_get_freq() bounds it the same way); the rows written are between
	 * the floor and this one
	 */
	top_row[d] = row_now[d] = MIN(rd(epss[d] + layout->perf_state), last);
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
			wr(epss[d] + layout->perf_state, row);
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
	bool held_for_silence = false;          /* (said once each time it begins) */

	for (;;) {
		unsigned which = 0, silent = 0;
		int32_t t = read_hottest(&which, &silent);

		hottest = t;
		tell_look(t, which);
		if (mode >= 2) {
			/*
			 * applied at every look, not only when the step changes: apply() writes only the groups that are not
			 * where the step puts them, so a level the firmware refused (an SCMI channel busy, a call that timed
			 * out) is asked for again at the next look -- a step held at the critical temperature is not lost to
			 * one failed call
			 */
			struct th_policy now = policy;
			uint32_t next;

			if (now.clear < now.trip && now.trip < now.critical) {
				in_force = now;
			}
			next = th_next_step(step, most, t, &in_force);
			/*
			 * nothing given back while a sensor that has answered before is silent: the hottest of the others may be
			 * cool, but the silent one may be the one that was hot -- the processors' own, under them -- and taking the
			 * others' word for it gave the rows back, a step a look, to a phone that was held back because that one was
			 * hot. Held more when another is hot, given back once every sensor that answers answers again.
			 */
			if (silent != 0 && next < step) {
				if (!held_for_silence) {
					say("thermal: %u sensor%s that answered before %s silent: held back %u of %u, nothing given back until "
					    "every one answers again\n", silent, silent == 1 ? "" : "s", silent == 1 ? "is" : "are", step, most);
				}
				held_for_silence = true;
				next = step;
			} else if (silent == 0) {
				held_for_silence = false;
			}
			step = next;
			apply(step);
			if (step != said_step) {
				char rows[48], degrees[16];

				say("thermal: %s C: held back %u of %u (rows %s)\n", tenths_text(t, degrees, sizeof(degrees)),
				    step, most, rows_text(row_now, rows, sizeof(rows)));
				said_step = step;
			}
		}
		if (looks++ % 10 == 0) {
			char degrees[16];

			say("thermal: hottest sensor %s C%s\n", tenths_text(t, degrees, sizeof(degrees)),
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
/* a threshold root sets: 0 to TH_THRESHOLD_MOST, or refused (EINVAL) and left as it was */
static int
sysctl_threshold SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg2)
	int32_t value = *(volatile int32_t *)arg1;
	int changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);

	if (error != 0 || !changed) {
		return error;
	}
	if (value < 0 || value > TH_THRESHOLD_MOST) {
		return EINVAL;
	}
	*(volatile int32_t *)arg1 = value;
	return 0;
}

SYSCTL_PROC(_hw_thermal, OID_AUTO, trip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &policy.trip, 0, sysctl_threshold, "I",
    "held back from, tenths of a degree");
SYSCTL_PROC(_hw_thermal, OID_AUTO, clear, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &policy.clear, 0, sysctl_threshold, "I",
    "given back at, tenths of a degree");
SYSCTL_PROC(_hw_thermal, OID_AUTO, critical, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &policy.critical, 0,
    sysctl_threshold, "I", "all the way at once, tenths of a degree");

/* ---- start and end ------------------------------------------------------------------------------- */

void
hakios_thermal_init(void)
{
	thread_t thread;
	int32_t value;

	PE_parse_boot_argn("thermal", &mode, sizeof(mode));
	if (mode == 0) {
		return;
	}
	this_board = hakios_board();
	if (this_board == NULL || this_board->thermal.tsens[0] == 0) {
		say("thermal: this board's file has no thermal { } (arch/arm64/boot/dts/NAME/board.dts): nothing read\n");
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
	if (policy.clear < 0 || policy.clear >= policy.trip || policy.trip >= policy.critical || policy.critical > TH_THRESHOLD_MOST) {
		say("thermal: 0 <= th_clear < th_trip < th_critical <= %d does not hold (%d, %d, %d): the defaults\n",
		    TH_THRESHOLD_MOST, policy.clear, policy.trip, policy.critical);
		policy = (struct th_policy){ 700, 620, 950 };
	}
	in_force = policy;
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
	/* (looking only, the groups' registers are read and never written: what the hardware allows, in tell_look) */
	layout = cpufreq_hw_layout(this_board);
	for (unsigned d = 0; d < groups && !scmi && this_board->cpufreq.epss != 0; d++) {
		epss[d] = ml_io_map(cpufreq_hw_domain(this_board, d), cpufreq_hw_map_size(this_board));
		read_table(d);
		if ((rd(epss[d] + layout->enable) & 1) == 0) {
			/* (as board/cpufreq.c leaves it, and Linux: looked at; its floor its row, so that row is never written) */
			say("thermal: group %u: the hardware is not in charge of its clock: looked at, never held back\n", d);
			floor_row[d] = top_row[d];
		}
		if (mode >= 2) {
			most = top_row[d] - floor_row[d] > most ? top_row[d] - floor_row[d] : most;
		}
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
	/* SCMI: nothing is asked here -- hakios_scmi_perf_quiesce(), called after this, asks each domain back to the level
	 * it was found at, held back or not */
	if (mode < 2 || step == 0 || scmi) {
		return;
	}
	for (unsigned d = 0; d < TH_DOMAINS; d++) {
		if (epss[d] != 0 && row_now[d] != top_row[d]) {
			wr(epss[d] + layout->perf_state, top_row[d]);
			row_now[d] = top_row[d];
		}
	}
	{
		char rows[48];

		say("thermal: at the run's end: the groups back at rows %s\n", rows_text(top_row, rows, sizeof(rows)));
	}
}
