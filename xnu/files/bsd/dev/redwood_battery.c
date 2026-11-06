/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * Redwood selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

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

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
redwood_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_NODE(_kern, OID_AUTO, redwood, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

static void
battery_poll(void)
{
	struct loader_battery sample;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_updates++;
}

/*
 * What the audio DSP's battery manager says (bsd/dev/redwood_adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that iOS reads (iokit/Kernel/RedwoodBattery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_redwood, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void RedwoodBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/RedwoodBattery.cpp */

void
redwood_battery_report(const struct redwood_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
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
	battery_source_dsp = 1;
	battery_updates++;
	RedwoodBatteryReport(b, sizeof(*b));
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
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
redwood_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
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
