/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * The phone selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
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
#include <sys/hakios.input.h>
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
hakios_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's: every driver hangs its sysctls there */
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
 * What the audio DSP's battery manager says (drivers/audio/hakios.adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that Darwin reads (drivers/power/hakios.battery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void HakiosBatteryReport(const void *state, unsigned size);     /* drivers/power/hakios.battery.cpp */

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	/* (rounded in 64 bits: the DSP's value near INT32_MAX plus 50 would overflow an int, and a full battery would read 0%) */
	int percent = (int)(((int64_t)b->percent_hundredths + 50) / 100);

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	/* (in 64 bits for the same reason: an overflow here would make a hot battery read as cold) */
	battery_temperature_c = (int)(((int64_t)b->temperature_hundredths + 50) / 100);
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
	HakiosBatteryReport(b, sizeof(*b));
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

/*
 * battery=2: the Redmi Note 13 4G (sapphiren). Its charger and its gauge are the PM7250B's, slave 2 of the
 * SPMI bus: the SMB5 charger at 0x1000 to 0x13ff and the QG gauge at 0x4800 (its device tree, qcom,pm7250b-smb5
 * and qcom,pm7250b-qg). Registers and scales as the same phone's other port uses them (ios-sapphire patches
 * 0206, 0208, 0213, 0325) and Linux's qpnp-smb5 and qpnp-qg. Read only: the charger works by itself.
 *
 * The percent is an ESTIMATE, as there is no Linux running the QG to keep its state: this cell's own
 * open-circuit curve (the 25 C column of its profile, "guanyu 5000 mAh") at the voltage with the current's
 * drop taken out, kept from jumping: a point at most a poll, never up while the battery gives current and
 * never down while it takes it, 99 until the charger says full.
 */
extern int hakios_pmic_read(unsigned sid, unsigned addr, uint8_t *out);

#define PM7250B 2

static bool
pmic_word(unsigned address, uint16_t *value)
{
	uint16_t previous = 0;

	for (unsigned attempt = 0; attempt < 4; attempt++) {
		uint8_t lo, hi;

		if (hakios_pmic_read(PM7250B, address, &lo) || hakios_pmic_read(PM7250B, address + 1, &hi)) {
			return false;
		}
		uint16_t next = (uint16_t)(lo | hi << 8);

		if (attempt != 0 && next == previous) {
			*value = next;
			return true;
		}
		previous = next;
	}
	*value = previous;
	return true;
}

static unsigned
ocv_percent(unsigned mv)
{
	static const unsigned volts[] = { 3000, 3325, 3474, 3563, 3625, 3671, 3696, 3704, 3709, 3720,
	    3736, 3753, 3766, 3778, 3789, 3797, 3806, 3816, 3829, 3844, 3860, 3879, 3892, 3909, 3932,
	    3962, 3985, 4004, 4041, 4083, 4129, 4178, 4230, 4282, 4331, 4377, 4422, 4452, 4495 };
	static const unsigned soc[] = { 0, 1, 2, 3, 4, 5, 6, 8, 10, 12,
	    14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 50, 52, 54,
	    56, 58, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 98, 100 };
	const unsigned n = sizeof(volts) / sizeof(volts[0]);

	if (mv <= volts[0]) {
		return 0;
	}
	for (unsigned i = 1; i < n; i++) {
		if (mv <= volts[i]) {
			return soc[i - 1] + (soc[i] - soc[i - 1]) * (mv - volts[i - 1]) / (volts[i] - volts[i - 1]);
		}
	}
	return 100;
}

static void
battery_pmic_thread(__unused void *parameter, __unused wait_result_t result)
{
	/* the arbiter is mapped by the buttons' driver (buttons=2), which starts beside this one */
	uint8_t probe;
	unsigned shown = 0, full_polls = 0, bad = 0, tick = 0;
	bool seeded = false;

	for (unsigned wait = 0; wait < 120 && hakios_pmic_read(PM7250B, 0x4804, &probe) == -1; wait++) {
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	for (;; tick++, delay_for_interval(2000, NSEC_PER_MSEC)) {
		uint8_t type = 0, present = 0, usb = 0, path = 0, charger = 0, error = 0, subtype = 0;
		uint16_t raw = 0, rawi = 0;
		static const unsigned vregs[] = { 0x48c0, 0x4880, 0x4890, 0x4870 };
		static const unsigned iregs[] = { 0x48c2, 0x4882, 0x4892, 0x4872 };
		unsigned mv = 0, which = 0;
		int ma = 0;
		bool have_v = false, have_i = false;

		if (hakios_pmic_read(PM7250B, 0x4804, &type) || hakios_pmic_read(PM7250B, 0x4808, &present) ||
		    hakios_pmic_read(PM7250B, 0x1310, &usb) || hakios_pmic_read(PM7250B, 0x110b, &path) ||
		    hakios_pmic_read(PM7250B, 0x1006, &charger) || hakios_pmic_read(PM7250B, 0x1007, &error)) {
			if (bad++ % 15 == 0) {
				printf("battery: the PM7250B does not answer (type %#x present %#x usb %#x charger %#x)\n", type, present, usb,
				    charger);
			}
			continue;
		}
		for (which = 0; which < 4 && !have_v; which++) {
			if (pmic_word(vregs[which], &raw) && raw != 0 && raw != 0x8000) {
				unsigned v = (unsigned)(((uint64_t)raw * 194637u) / 1000000u);

				if (v >= 2800 && v <= 4600) {
					mv = v;
					have_v = true;
					break;
				}
			}
		}
		if (have_v && hakios_pmic_read(PM7250B, 0x4805, &subtype) == 0 && (subtype == 3 || subtype == 4) &&
		    pmic_word(iregs[which < 4 ? which : 0], &rawi) && rawi != 0x8000) {
			int signed_raw = (rawi & 0x8000u) ? (int)rawi - 65536 : (int)rawi;

			ma = (int)(-((int64_t)signed_raw * (subtype == 3 ? 152588 : 305176)) / 1000000);
			have_i = true;
		}
		bool external = (usb & 0x10u) != 0;
		bool capable = external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
		unsigned state = charger & 7u;
		bool charging = capable && state >= 1 && state <= 4;

		/* full: the charger says so (state 5), or -- it often never does, staying in its last phase -- the voltage has
		 * stood at the top of the cell's curve for ~20 s with the charger on */
		bool top = external && have_v && mv >= 4440;
		full_polls = ((external && state == 5 && !(error & 2u)) || top) ? full_polls + 1 : 0;
		bool full = full_polls >= (top ? 10u : 3u);

		if (!have_v) {
			if (tick % 15 == 0) {
				printf("battery: the PM7250B gives no voltage (gauge idle): usb %#x chgr %#x -> external=%d charging=%d\n",
				    usb, charger, external, charging);
			}
			continue;
		}
		int ocv = (int)mv - (have_i ? ma * 15 / 100 : 0);     /* ma > 0 charging: the voltage stands above the open circuit */
		unsigned target = ocv_percent(ocv < 3000 ? 3000 : (unsigned)ocv);

		if (!seeded) {
			seeded = true;
			shown = target;
		} else if (target > shown && !(have_i && ma < -20)) {
			shown++;
		} else if (target < shown && !(have_i && ma > 20)) {
			shown--;
		}
		if (shown > 99 && external && !full) {
			shown = 99;
		}
		if (full) {
			shown = 100;
		}
		struct hakios_battery_state b = {
			.size = sizeof(b), .percent_hundredths = (int32_t)shown * 100, .voltage_uv = (int32_t)mv * 1000,
			.current_ua = ma * 1000, .temperature_hundredths = 2500, .present = (present & 1u) != 0,
			.external = external, .status = charging ? 1 : full ? 4 : external ? 3 : 2,
		};

		hakios_battery_report(&b);
		if (tick % 15 == 0) {
			printf("battery: PM7250B %u mV (%s) %d mA -> %u%% (open circuit %u%%); usb %#x path %#x charger %#x error %#x: "
			    "external=%d charging=%d full=%d\n", mv, have_i ? "current read" : "no current", ma, shown, target, usb,
			    path, charger, error, external, charging, full);
		}
	}
}

int
hakios_battery_summary(char *buffer, size_t size)
{
	return snprintf(buffer, size, "SOC=%d%% V=%d mV I=%d mA T=%d C present=%d charger=%d charging=%d status=%d (%llu updates, %llu failed)",
	    battery_soc_percent, battery_voltage_mv, battery_current_ma, battery_temperature_c, battery_present,
	    battery_charger_present, battery_charging, battery_charger_action, battery_updates, battery_failures);
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	/*
	 * battery_level=N (percent): for a phone whose battery gauge has no driver here yet (the Redmi Note 13's Qualcomm "QG"):
	 * Darwin is told the charge the run started with, as it was in the phone's own Android just before, and that the phone is
	 * on battery. A level that does not follow the battery: it is not a measurement.
	 */
	{
		uint32_t level = 0;

		if (PE_parse_boot_argn("battery_level", &level, sizeof(level)) && level <= 100) {
			struct hakios_battery_state fixed = {
				.size = sizeof(fixed), .percent_hundredths = (int32_t)level * 100, .voltage_uv = 3800000,
				.temperature_hundredths = 3000, .present = 1, .external = 0, .status = 2,
			};

			hakios_battery_report(&fixed);
			printf("battery: a fixed level of %u%% (battery_level=), no gauge\n", level);
			return;
		}
	}
	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	if (enabled == 2) {
		/* the PM7250B's own gauge and charger (the Redmi Note 13 4G): nothing from the loader */
		if (kernel_thread_start(battery_pmic_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
			printf("battery: the PM7250B's gauge and charger, polled every 2 seconds (battery=2)\n");
		}
		return;
	}
	if (!PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) || shared_pa == 0) {
		printf("battery: xnuloader did not provide battery_shared_pa\n");
		return;
	}
	/* The cache must lie inside the one page that is mapped: an address near its end would be read past the mapping. */
	if ((shared_pa & (LOADER_PAGE_SIZE - 1)) + sizeof(struct loader_battery) > LOADER_PAGE_SIZE) {
		printf("battery: battery_shared_pa %#llx: the cache would cross the end of its page\n", shared_pa);
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
