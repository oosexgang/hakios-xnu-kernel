/*
 * Keeping the phone from getting hot, for any device of the Qualcomm kind: one driver with what differs
 * between devices in a table (th_profiles below), the arithmetic in hakios_thermal_core.h (tested on the
 * host: renoir/tests/thermal_test.c, and the driver itself: renoir/tests/thermal_driver_test.c).
 *
 * Every second the SoC's temperature sensors are read (both TSENS controllers, every sensor that has a
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
 * Adding a device: its TSENS "tsens_tm_physical" addresses from its device tree, its EPSS ("cpufreq-hw")
 * address, and the floors. GPU frequency is not touched here: the GPU's speed belongs to its GMU
 * (bsd/dev/redwood_adreno.c, boot-arg adreno_speed); hakios_thermal_step() tells it how far to go.
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

#include "hakios_thermal_core.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool redwood_board_is(const char *name);
extern void loader_log_putc(char c);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void hakios_backlight_set_cap(uint32_t level);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

/* ---- what differs between devices ----------------------------------------------------------------- */

#define TH_CONTROLLERS          2
#define TH_SENSORS              16              /* a TSENS controller has up to 16 */
#define TH_DOMAINS              3

struct th_profile {
	const char *board;
	uint32_t tsens[TH_CONTROLLERS];         /* "tsens_tm_physical" of tsens0, tsens1 (0: none) */
	uint32_t epss, epss_size;               /* "cpufreq-hw": a block of epss_size a group */
	uint32_t floor_mhz[TH_DOMAINS];         /* the slowest each group is held back to */
};

static const struct th_profile th_profiles[] = {
	/* SM7350 (shima) and SM7325 (yupik), both lahaina-family: tsens at 0xc263000/0xc265000, EPSS at 0x18591000 */
	{ "renoir", { 0x0c263000, 0x0c265000 }, 0x18591000, 0x1000, { 1000, 1100, 1100 } },
	{ "redwood", { 0x0c263000, 0x0c265000 }, 0x18591000, 0x1000, { 1000, 1100, 1100 } },
};

#define TSENS_STATUS(sensor)    (0xa0 + 4 * (sensor))
#define EPSS_FREQ_TABLE         0x100
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000

static struct th_profile profile;
static uint32_t mode;
static struct th_policy policy = { 700, 620, 950 };
static vm_offset_t tsens[TH_CONTROLLERS], epss[TH_DOMAINS];
static uint32_t top_row[TH_DOMAINS], floor_row[TH_DOMAINS], row_now[TH_DOMAINS];
static volatile int32_t hottest = TH_NONE;
static volatile uint32_t step, most;

#ifndef TH_TEST_MMIO
static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
#if defined(__arm64__) || defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#endif
}
#else
/* renoir/tests/thermal_driver_test.c: the registers are a model of the hardware's */
extern uint32_t th_test_rd(vm_offset_t address);
extern void th_test_wr(vm_offset_t address, uint32_t value);
#define rd(a)           th_test_rd(a)
#define wr(a, v)        th_test_wr((a), (v))
#endif

/* the hottest reading of every sensor of both controllers */
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
		if (floor_row[d] == UINT32_MAX && khz >= profile.floor_mhz[d] * 1000) {
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

		if (epss[d] != 0 && row != row_now[d]) {
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
				say("thermal: %d.%d C: held back %u of %u (rows %u %u %u)\n", t / 10, t % 10 < 0 ? -(t % 10) : t % 10,
				    step, most, row_now[0], row_now[1], row_now[2]);
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
	bool known = false;

	if (!PE_parse_boot_argn("thermal", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(th_profiles) / sizeof(th_profiles[0]); i++) {
		if (redwood_board_is(th_profiles[i].board)) {
			profile = th_profiles[i];
			known = true;
			break;
		}
	}
	if (!known) {
		say("thermal: no profile for this board: nothing read\n");
		mode = 0;
		return;
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
		tsens[c] = profile.tsens[c] ? ml_io_map(profile.tsens[c], 0x200) : 0;
	}
	most = 0;
	for (unsigned d = 0; d < TH_DOMAINS && mode >= 2; d++) {
		epss[d] = ml_io_map(profile.epss + d * profile.epss_size, profile.epss_size);
		read_table(d);
		most = top_row[d] - floor_row[d] > most ? top_row[d] - floor_row[d] : most;
	}
	say("thermal: %s: trip %d.%d C, clear %d.%d C, critical %d.%d C; the groups at rows %u %u %u, floors %u %u %u%s\n",
	    profile.board, policy.trip / 10, policy.trip % 10, policy.clear / 10, policy.clear % 10, policy.critical / 10,
	    policy.critical % 10, top_row[0], top_row[1], top_row[2], floor_row[0], floor_row[1], floor_row[2],
	    mode >= 2 ? "" : " (thermal=1: looking only)");
	if (kernel_thread_start(th_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "thermal");
		thread_deallocate(thread);
	}
}

/* At the run's end: the groups back where they were found. */
void
hakios_thermal_quiesce(void)
{
	if (mode < 2 || step == 0) {
		return;
	}
	for (unsigned d = 0; d < TH_DOMAINS; d++) {
		if (epss[d] != 0 && row_now[d] != top_row[d]) {
			wr(epss[d] + EPSS_PERF_STATE, top_row[d]);
			row_now[d] = top_row[d];
		}
	}
	say("thermal: at the run's end: the groups back at rows %u %u %u\n", top_row[0], top_row[1], top_row[2]);
}
