/*
 * The battery, as the phone's resident UEFI reports it (feature battery-uefi).
 *
 * On a phone whose charger is behind the audio DSP (qcom,battery-charger over PMIC GLINK) rather
 * than a fuel gauge of its own, the resident UEFI carries Qualcomm's charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 *
 * What is read is only believed while it is new: a cache whose loader has stopped polling still looks whole, and
 * an audio DSP that has stopped answering simply says nothing more. The source in use that has said nothing new
 * for BATTERY_QUIET_SECONDS is taken for quiet: its temperature is no longer handed out (-273), which the
 * flashlight's guard takes for trouble (hakios_battery_went_quiet()), and this is said once, and once again when
 * it speaks again. A quiet audio DSP gives way to the loader's cache while that is new.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "battery_core.h"
/* what is said goes to the loader's log as it is said, and to the kernel's (board/log.h) */
#include "log.h"
#define printf hakios_say

#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;

SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the values HakiOS' drivers keep");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

/* what was loaded so far is loaded before what follows (the loader writes the cache on another core) */
static inline void
barrier(void)
{
#if defined(__arm64__) || defined(__aarch64__)
	__asm__ volatile ("dmb ishld" ::: "memory");
#else
	__asm__ volatile ("" ::: "memory");     /* (the host tests on another processor) */
#endif
}

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		barrier();
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		barrier();
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

static int battery_source_dsp;

/*
 * When the source in use last said something new (mach_absolute_time): the loader's cache when its polls or its
 * sequence have moved on since the last look, the audio DSP's battery manager at every report. Written under
 * battery_report_lock; read without it (a 64-bit word: read whole).
 */
#define BATTERY_QUIET_SECONDS   60
static uint64_t battery_fresh_at;
static bool battery_heard;              /* the source in use has said something in this run */
static bool battery_quiet_said;         /* that it went quiet was said, and not yet that it speaks again */
static bool battery_dsp_gave_way;       /* the audio DSP went quiet and the loader's cache was passed on again */
static uint32_t loader_polls, loader_sequence;  /* the cache's at the last whole look */

static const char *
battery_source_name(void)
{
	return battery_source_dsp ? "the audio DSP's battery manager" : "xnuloader's cache";
}

/* nothing new from the source in use for BATTERY_QUIET_SECONDS, at `now` */
static bool
battery_silent_at(uint64_t now)
{
	uint64_t limit, fresh_at = __atomic_load_n(&battery_fresh_at, __ATOMIC_RELAXED);

	nanoseconds_to_absolutetime(BATTERY_QUIET_SECONDS * NSEC_PER_SEC, &limit);
	return __atomic_load_n(&battery_heard, __ATOMIC_RELAXED) && now > fresh_at && now - fresh_at >= limit;
}

/* The same, and said when it changes: once on going quiet, once on speaking again. */
static bool
battery_quiet(void)
{
	bool quiet = battery_silent_at(mach_absolute_time());

	if (__atomic_exchange_n(&battery_quiet_said, quiet, __ATOMIC_RELAXED) != quiet) {
		if (quiet) {
			printf("battery: nothing new from %s for %d s: the battery's temperature is taken for unknown until there is\n",
			    battery_source_name(), BATTERY_QUIET_SECONDS);
		} else {
			printf("battery: %s has news again: the battery's temperature is known again (%d C)\n", battery_source_name(),
			    battery_temperature_c);
		}
	}
	return quiet;
}

/*
 * A temperature that no cell has -- a thermistor open or shorted, a word of the firmware's status that is something
 * else (the loader passes it on unchecked), a number of the DSP's near the int's end -- is not a temperature: it is
 * taken for "not known" (-273). Taken as it was, a cell said to be at -400 C was cool enough for the flashlight's
 * guard, one at 6553 C kept the light off for good, and one near the int's end was multiplied past it for Darwin.
 */
#define BATTERY_COLDEST_C       (-40)
#define BATTERY_HOTTEST_C       100
static bool battery_temperature_was_known;      /* a temperature that can be was taken in this run */

static int
battery_plausible_c(int c)
{
	if (c < BATTERY_COLDEST_C || c > BATTERY_HOTTEST_C) {
		return -273;
	}
	__atomic_store_n(&battery_temperature_was_known, true, __ATOMIC_RELAXED);
	return c;
}

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known, or no longer */
int
hakios_battery_temperature(void)
{
	return battery_quiet() ? -273 : battery_temperature_c;
}

/*
 * For the flashlight's guard: a temperature known earlier in this run is no longer reported -- its source went
 * quiet holding one, or what it says now cannot be a temperature (battery_plausible_c()). Never true where no
 * battery was ever heard of, nor where no temperature that can be was ever said.
 */
bool
hakios_battery_went_quiet(void)
{
	return __atomic_load_n(&battery_temperature_was_known, __ATOMIC_RELAXED) &&
	       (battery_quiet() || battery_temperature_c == -273);
}

static int battery_shown_tenths = -1;   /* what iOS is shown, tenths of a percent (-1: nothing yet) */
/*
 * Boot-arg battery_shown=N (a test): iOS is shown N percent whatever the cell says, the rest (voltage, current, the
 * charger, the temperature) as it is -- for iOS's own low-battery alert at 19, an alert to look at on purpose
 * (the Metal plug-in's binning once left one out of most frames while a finger moved on it). The log's battery lines say N
 * as well.
 */
static uint32_t battery_shown_override = ~0u;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_shown, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_shown_tenths, 0,
    "the charge iOS is shown, tenths of a percent: the firmware's when the voltage agrees, else the voltage's (-1 if none)");

extern void HakiosBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/BoardBattery.cpp */

/*
 * How long the charge lasts, for Darwin's power source to say beside the percentage (battery_core.h has what is
 * said when; after the idea of the OnePlus Nord 2 port's piece 0701). The current of every reading that is handed
 * on, the loader's or the audio DSP's, goes into the one average; the charge is the gauge's count of it, or the full
 * charge the gauge has learnt at the percentage shown -- so a phone whose only source is the loader's cache, which
 * has neither, is never told a time, and one whose audio DSP reports is.
 * Boot-arg battery_time=0: nothing is averaged and nothing of it is said to Darwin, whose power source is then as
 * it was before this (no TimeRemaining, AvgTimeToEmpty or AvgTimeToFull at all).
 */
static struct bat_time battery_time;
static uint32_t battery_time_wanted = ~0u;      /* (~0: the boot-arg is not read yet) */
static int battery_minutes = BAT_TIME_UNKNOWN, battery_average_ma;
static int battery_time_said_way = 2;           /* which way the charge went when a time was last said in the log */
static int battery_full_uah;                    /* (below, with the audio DSP's other figures) */

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_minutes, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_minutes, 0,
    "minutes until the battery is empty, or full while it takes charge; -1 not known, -2 not worked out (battery_time=0)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_average_ma, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_average_ma, 0,
    "the battery's current averaged over about a minute, in milliamps, more than 0 while it takes charge");

/* What is handed on gets its minutes, and the average its look. Under battery_report_lock, as all of a report. */
static void
battery_time_tell(struct hakios_battery_state *state)
{
	uint64_t now_ns;
	int way, minutes;

	if (battery_time_wanted == ~0u) {
		battery_time_wanted = 1;
		PE_parse_boot_argn("battery_time", &battery_time_wanted, sizeof(battery_time_wanted));
	}
	if (!battery_time_wanted) {
		state->minutes_to_empty = state->minutes_to_full = battery_minutes = BAT_TIME_OFF;
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	way = bat_time_way(state->external, state->status);
	bat_time_look(&battery_time, way, state->current_ua, now_ns / NSEC_PER_MSEC);
	/* (no battery: no time. The average is the look's own way, which is this reading's unless its current cannot be) */
	minutes = state->present && battery_time.way == way ? bat_time_minutes(&battery_time,
	    bat_time_charge_uah(state->charge_uah, state->full_uah > 0 ? state->full_uah : battery_full_uah,
	    state->percent_hundredths), state->full_uah > 0 ? state->full_uah : battery_full_uah) : BAT_TIME_UNKNOWN;
	battery_average_ma = battery_time.average_ua / 1000;
	if (minutes >= 0 && battery_time_said_way != way) {
		/* (once each time the charge turns: what Darwin is told can be read beside it, userland's battery_test) */
		battery_time_said_way = way;
		printf("battery: at %d mA on average, %d min until it is %s: said to Darwin from here on\n", battery_average_ma,
		    minutes, way > 0 ? "full" : "empty");
	}
	battery_minutes = minutes;
	state->minutes_to_empty = way < 0 ? minutes : BAT_TIME_UNKNOWN;
	state->minutes_to_full = way > 0 ? minutes : BAT_TIME_UNKNOWN;
}

/*
 * HakiosBatteryReport() takes one caller at a time, and two threads call it: the loader's readings here and the audio DSP's
 * battery manager (hakios_battery_report()). Both at once had the service's properties replaced under the other's
 * update, or two power sources made at the first reading. The lock is held around each report, and the readings
 * here look again under it whether the DSP has taken over. The values kept for the sysctls and for the flashlight's
 * guard are taken under it too, with the look at which source is in use: the loader's readings took theirs after
 * the look, unlocked, and a report of the DSP's that came in between -- its first, or its first after it had gone
 * quiet -- was written over by the cache's, the DSP then the source and its temperature, its charge not the ones
 * kept until it reported again.
 */
static LCK_GRP_DECLARE(battery_lock_group, "hakios-battery");
static LCK_MTX_DECLARE(battery_report_lock, &battery_lock_group);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

/*
 * The firmware's reading as iOS's power source wants it (iokit/Kernel/BoardBattery.cpp): the charge judged by
 * hakios_battery_core.h, the current the other way round (iOS's is more than 0 while the battery takes charge), and
 * charging only while charge goes in -- on a computer's port the phone can draw more than the port gives, and is
 * then on a charger and not charging, as iOS says of a battery on hold. Not while the audio DSP's battery manager
 * reports (hakios_battery_report): it knows better.
 */
static void
battery_tell_ios(const struct loader_battery *sample)
{
	const int mv = (sample->flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample->voltage_mv : -1;
	const int external = (sample->flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	/* (a current that cannot be is none: battery_core.h, BAT_MOST_MA) */
	const int ma = sample->current_ma >= -BAT_MOST_MA && sample->current_ma <= BAT_MOST_MA ? sample->current_ma : 0;
	const int gaining = external && ma < BAT_GAINING_MA;
	struct hakios_battery_state state;

	if (battery_source_dsp) {
		return;
	}
	battery_shown_tenths = bat_shown_tenths(battery_shown_tenths,
	    bat_estimate_tenths(sample->soc_percent, (sample->flags & LOADER_BATTERY_SOC_ESTIMATED) != 0, mv, ma), gaining);
	if (battery_shown_override <= 100) {
		battery_shown_tenths = (int)battery_shown_override * 10;
	}
	memset(&state, 0, sizeof(state));
	state.size = sizeof(state);
	state.percent_hundredths = battery_shown_tenths * 10;
	state.voltage_uv = mv > 0 ? mv * 1000 : 0;
	state.current_ua = -ma * 1000;
	state.temperature_hundredths = battery_plausible_c(sample->temperature_c) * 100;
	state.present = (sample->flags & LOADER_BATTERY_PRESENT) != 0;
	state.external = external;
	/* (POWER_SUPPLY_STATUS_*: charging 1, discharging 2, not charging 3, full 4) */
	state.status = !external ? 2 : gaining ? 1 : battery_shown_tenths >= 1000 ? 4 : 3;
	lck_mtx_lock(&battery_report_lock);
	if (!battery_source_dsp) {
		battery_time_tell(&state);
		HakiosBatteryReport(&state, sizeof(state));
	}
	lck_mtx_unlock(&battery_report_lock);
	{
		uint64_t now_ns;

		/* (a line a look, beside the thermal ones: darwin/tools/frames.py shows what the battery did in each phase) */
		absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		hakios_tl("tl: b at %llu shown %d mv %d ma %d c %d external %d\n", now_ns / 1000, battery_shown_tenths, mv,
		    -ma, battery_plausible_c(sample->temperature_c), external);
	}
}

/*
 * A look at the loader's cache. It is new when the loader has polled since the last look (its polls or its sequence
 * moved on; the loader polls every 5 seconds while the kernel runs, except while the kernel's own UFS list is in).
 * While the audio DSP's battery manager reports, the cache is only looked at: the DSP knows better, unless it has
 * gone quiet while the cache is new -- then the cache is passed on again, until the DSP speaks again.
 */
static void
battery_poll(void)
{
	struct loader_battery sample;
	bool fresh, taken_over = false;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return;
	}
	lck_mtx_lock(&battery_report_lock);
	fresh = loader_polls != sample.polls || loader_sequence != sample.sequence || !battery_heard;
	loader_polls = sample.polls;
	loader_sequence = sample.sequence;
	if (battery_source_dsp && fresh && battery_silent_at(mach_absolute_time())) {
		battery_source_dsp = 0;
		battery_dsp_gave_way = taken_over = true;
		/* (what is shown goes on from the DSP's last figure, not from the cache's of before the DSP spoke) */
		battery_shown_tenths = battery_soc_percent * 10;
	}
	if (battery_source_dsp) {
		lck_mtx_unlock(&battery_report_lock);
		return;
	}
	if (fresh) {
		__atomic_store_n(&battery_fresh_at, mach_absolute_time(), __ATOMIC_RELAXED);
		__atomic_store_n(&battery_heard, true, __ATOMIC_RELAXED);
	}
	/* (under the lock, with the look above: see battery_report_lock) */
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = battery_plausible_c(sample.temperature_c);
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_updates++;
	lck_mtx_unlock(&battery_report_lock);
	if (taken_over) {
		/* (this says it, not battery_quiet(): the source is another now, and it is not quiet) */
		__atomic_store_n(&battery_quiet_said, false, __ATOMIC_RELAXED);
		printf("battery: nothing new from the audio DSP's battery manager for %d s: xnuloader's cache, which is new, is "
		    "passed on again\n", BATTERY_QUIET_SECONDS);
	}
	battery_tell_ios(&sample);
}

/*
 * What the audio DSP's battery manager says (bsd/dev/board/adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that Darwin reads (iokit/Kernel/BoardBattery.cpp).
 */
static int battery_design_uah, battery_cycles;  /* (and battery_full_uah, above) */

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;
	const int hundredths = b->temperature_hundredths;
	struct hakios_battery_state told = *b;  /* what is handed on: the DSP's, and how long the charge lasts */
	bool back;

	/* (under the lock, as the loader's readings: see battery_report_lock) */
	lck_mtx_lock(&battery_report_lock);
	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	/*
	 * (looked at before it is rounded: near the int's end, adding to it went past. Rounded half away from 0: C's division
	 * goes toward 0, and + 50 made -1.00 C 0 C, -40.00 C -39 C)
	 */
	battery_temperature_c = hundredths >= BATTERY_COLDEST_C * 100 && hundredths <= BATTERY_HOTTEST_C * 100 ?
	    battery_plausible_c((hundredths + (hundredths < 0 ? -50 : 50)) / 100) : -273;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	back = !battery_source_dsp && battery_dsp_gave_way;
	battery_dsp_gave_way = false;
	battery_source_dsp = 1;
	__atomic_store_n(&battery_fresh_at, mach_absolute_time(), __ATOMIC_RELAXED);
	__atomic_store_n(&battery_heard, true, __ATOMIC_RELAXED);
	battery_updates++;
	battery_time_tell(&told);
	HakiosBatteryReport(&told, sizeof(told));
	lck_mtx_unlock(&battery_report_lock);
	if (back) {
		printf("battery: the audio DSP's battery manager speaks again: its readings are passed on again\n");
	}
	(void)battery_quiet();          /* (that it speaks again, if it had gone quiet) */
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		(void)battery_quiet();  /* (said here too: nothing else may ask) */
		if (tick % 12 == 0) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!hakios_board_has(HAKIOS_HAS_BATTERY_UEFI)) return;
	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	PE_parse_boot_argn("battery_shown", &battery_shown_override, sizeof(battery_shown_override));
	/* battery_shared_pa=ADDRESS: the page the loader shares the charger's state in (the loader gives it) */
	if (!PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) || shared_pa == 0) {
		printf("battery: xnuloader did not provide battery_shared_pa\n");
		return;
	}
	page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
	shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
	if (shared_battery->magic != LOADER_BATTERY_MAGIC || shared_battery->version != LOADER_BATTERY_VERSION ||
	    shared_battery->size < sizeof(struct loader_battery)) {
		printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
		    shared_battery->magic, shared_battery->version, shared_battery->size);
		return;
	}
	printf("battery: Qualcomm UEFI cache at pa %#llx; polling every 5 seconds\n", shared_pa);
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
