/*
 * The SoC's temperature sensors: sysctl kern.hakios.thermal_*, a line in the log, and a stop for a phone that is
 * getting too hot to be left alone. Only with boot-arg thermal=1.
 *
 * What is here. The chip has two blocks of temperature sensors ("TSENS", which Linux's tsens-v2.c drives). redwood
 * (SM7325; dt tsens@c222000 and tsens@c223000, compatible qcom,tsens26xx): 15 sensors in the first (the CPU cores, the
 * CPU subsystem, the always-on block) and 12 in the second (the GPU, the neural processors, video, memory, the modem,
 * the camera). zeus (SM8450, devices/zeus/drivers/power/hakios.thermal.sm8450.c, the same file with THERMAL_SM8450;
 * dt tsens@c263000 and tsens@c265000, qcom,tsens-v2): 16 and 16, the big cores (cpu-1-0..8) and the GPU in the first,
 * the small cores (cpu-0-0..3), the modem, video and the camera in the second. On both, the blocks measure by themselves, all the time, and keep the last reading of each sensor in
 * a status word that anyone may read: bits [11:0] the temperature in tenths of a degree, signed, and bit 21 set
 * when there is a reading. The sensors are only read (one 32-bit load each): no write, no interrupt, no threshold set. Calibration is the firmware's: the blocks are calibrated from the
 * fuses by the secure side before the operating system runs (Linux's tsens does it too, from the QFPROM, for its
 * own use of the raw codes; the status words used here are already degrees).
 *
 * What a temperature is worth. A reading is believed when it has its valid bit and lies between -40 and 160
 * degrees; anything else is a sensor that is not connected, or not yet measured, and is left out of the maximum.
 * The table of sensors (names, which block, which number, and the temperatures at which the vendor slows
 * things down and gives up) is the phone's own device tree's thermal-zones, made by tools/dt-thermal.py: the
 * numbers are the vendor's, not ours.
 *
 * What it does with them.
 *   - every thermal_poll milliseconds (default 2000) it reads all sensors;
 *   - sysctl kern.hakios.thermal_max_mc (the hottest, in millidegrees), thermal_hottest (its name),
 *     thermal_sensors ("cpu-0-0=41.2 ... " in degrees), thermal_reads;
 *   - a line in the log (and the loader's log) every thermal_log seconds (default 60) and at once when the
 *     hottest sensor crosses a multiple of 5 degrees upwards from 60;
 *   - the brake: for each group of cores (drivers/power/hakios.cpufreq.c), the smallest headroom of the sensors
 *     over it (degrees to each sensor's own "slow down" trip, the vendor's 110 C for the cores). Within
 *     thermal_throttle degrees of the trip (default 15) one row of the group's frequency table is held back, and one
 *     more for every thermal_step degrees closer (default 3), never below a third of the top clock
 *     (hakios_cpufreq_cap(): the EPSS row written, nothing else). It lets go one row at a time, and only 2 degrees
 *     after: no flapping. A line in the log at each change ("thermal: group 1: ... rows held back: 2419 MHz");
 *     sysctl kern.hakios.thermal_brake ("g:rows/MHz ..."), thermal_brakes (how many changes), thermal_headroom;
 *   - the pressure level for iOS, sysctl kern.hakios.thermal_pressure, in OSThermalPressureLevel's numbers: 0 nominal
 *     outside the margin, 10 light in its first half, 20 moderate in its second half, 30 heavy past the trip, 40
 *     trapping 10 degrees past it (with the same 2 degrees of hysteresis). userland/src/thermal_notify.c posts it as
 *     com.apple.system.thermalpressurelevel, which NSProcessInfo.thermalState and OSThermalNotification read;
 *   - the stop: when the hottest valid sensor reads at least thermal_stop degrees three times in a row (default
 *     105; thermal_stop=0 turns the stop off), it says so and hands control back to the boot loader the way
 *     "xl stop" does (the GPU, Wi-Fi and audio DSP given back first). It is the last line, for a phone left running
 *     unattended, behind the brake; a stop of the run, not a repair: a hot phone is hot for minutes after.
 *
 * Status (what has and has not been seen): RUN ON THE PHONE (POCO X5 Pro, 2026-10-27, four runs): the status-word layout
 * (from Linux's tsens-v2.c as remembered, the same as drivers/gpu/hakios.adreno.c reads for the GPU's two sensors) is right:
 * 27 of 27 sensors have a reading, 33.3 to 39.5 degrees on a phone at rest, the CPU cores a degree or two above the rest, the
 * values move between runs; kern.hakios.thermal_* are read from user space (userland/src/hello.c with hello_wait=). NOT yet
 * seen: the stop (thermal_stop) firing, a phone under load, a reading above 40 degrees. The sensors are the device tree's; whether
 * the phone's own tree is the reference one (devices/redwood/prebuilt/device-tree.dtb) was not compared sensor by sensor.
 * zeus (Xiaomi 12 Pro, 2026-11-14, logs 211921 and 213706): 32 of 32 sensors read; the big cores at 93-95 C at boot and under
 * a load of every core. With thermal_throttle=70 (to see it work at once) the brake held group 0 at 729 MHz, group 1 at 1209
 * and group 2 at 1728, and the pressure went to 20 and 30, which iOS got (thermal_notify: the notify state 30,
 * NSProcessInfo.thermalState critical). With the default 15 it engaged once at 95 C (group 1 2419 MHz, group 2 2841,
 * pressure 10, thermalState fair) and let go again. The stop not seen firing on either phone.
 * Not run in QEMU, and thermal=1 must not be given there: it has no such blocks, and a load from an address it does not map faults.
 *
 * Boot-args:
 *   thermal=1          read the sensors
 *   thermal_poll=<ms>  how often (200 to 60000; 2000)
 *   thermal_log=<s>    a summary line this often (0: only on a rise; 60)
 *   thermal_stop=<C>   stop the run at this temperature (0: never; 105)
 *   thermal_throttle=<C>  the brake starts this far below a sensor's trip (0: no brake; 0 to 100; 15)
 *   thermal_step=<C>   one more row held back every so many degrees (1 to 20; 3)
 *   thermal_tm0=<pa>, thermal_tm1=<pa>  the two blocks' readings, when this is not the reference phone
 *                      (0xc263000 and 0xc265000)
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void loader_return(void);
extern void hakios_var_save_on_return(void);
extern void hakios_adreno_stop(void);
extern void hakios_wifi_stop(void);
extern void hakios_adsp_stop(void);

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[700];                                 /* the line of all the sensors is the long one */
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define TM_SIZE                 0x200
#define TM_STATUS(sensor)       (0xa0 + 4 * (sensor))           /* one word a sensor */
#define TM_VALID                (1u << 21)
#define TEMP_MIN_TENTHS         (-400)
#define TEMP_MAX_TENTHS         1600

/*
 * Made by tools/dt-thermal.py from the phone's device tree: name, block, sensor, slow down at, gives up at (millidegrees);
 * the last column is ours: which groups of cores (drivers/power/hakios.cpufreq.c: 0 cores 0-3, 1 cores 4-6, 2 core 7)
 * the sensor sits on, by its name: cpu-0-* the small cores, cpu-1-* the big ones (more sensors than cores, and the tree
 * does not say which core each is under, so both big groups), cpuss-* the subsystem around them all.
 */
#define SMALL   (1u << 0)
#define BIG     (1u << 1 | 1u << 2)
#define ALL     (SMALL | BIG)
static const struct sensor_info {
	const char *    name;
	uint32_t        tm;
	uint32_t        number;
	int32_t         slow_mc;
	int32_t         limit_mc;
	uint32_t        groups;
} sensors[] = {
#ifdef THERMAL_SM8450
	/* zeus (Xiaomi 12 Pro, SM8450): two blocks of 16; the big cores' sensors in the first, the small ones' in the second */
	{ "aoss-0", 0xc263000, 0, 115000, 125000, 0 },
	{ "cpuss-0", 0xc263000, 1, 115000, 125000, ALL },
	{ "cpuss-1", 0xc263000, 2, 115000, 125000, ALL },
	{ "cpuss-2", 0xc263000, 3, 115000, 125000, ALL },
	{ "cpuss-3", 0xc263000, 4, 115000, 125000, ALL },
	{ "cpu-1-0", 0xc263000, 5, 110000, 125000, BIG },
	{ "cpu-1-1", 0xc263000, 6, 110000, 125000, BIG },
	{ "cpu-1-2", 0xc263000, 7, 110000, 125000, BIG },
	{ "cpu-1-3", 0xc263000, 8, 110000, 125000, BIG },
	{ "cpu-1-4", 0xc263000, 9, 110000, 125000, BIG },
	{ "cpu-1-5", 0xc263000, 10, 110000, 125000, BIG },
	{ "cpu-1-6", 0xc263000, 11, 110000, 125000, BIG },
	{ "cpu-1-7", 0xc263000, 12, 110000, 125000, BIG },
	{ "cpu-1-8", 0xc263000, 13, 110000, 125000, BIG },
	{ "gpuss-0", 0xc263000, 14, 95000, 125000, 0 },
	{ "gpuss-1", 0xc263000, 15, 95000, 125000, 0 },
	{ "aoss-1", 0xc265000, 0, 115000, 125000, 0 },
	{ "cpu-0-0", 0xc265000, 1, 110000, 125000, SMALL },
	{ "cpu-0-1", 0xc265000, 2, 110000, 125000, SMALL },
	{ "cpu-0-2", 0xc265000, 3, 110000, 125000, SMALL },
	{ "cpu-0-3", 0xc265000, 4, 110000, 125000, SMALL },
	{ "nspss-0", 0xc265000, 5, 95000, 125000, 0 },
	{ "nspss-1", 0xc265000, 6, 95000, 125000, 0 },
	{ "nspss-2", 0xc265000, 7, 95000, 125000, 0 },
	{ "video", 0xc265000, 8, 115000, 125000, 0 },
	{ "ddr", 0xc265000, 9, 90000, 125000, 0 },
	{ "mdmss-0", 0xc265000, 10, 102000, 125000, 0 },
	{ "mdmss-1", 0xc265000, 11, 102000, 125000, 0 },
	{ "mdmss-2", 0xc265000, 12, 102000, 125000, 0 },
	{ "mdmss-3", 0xc265000, 13, 102000, 125000, 0 },
	{ "camera-0", 0xc265000, 14, 115000, 125000, 0 },
	{ "camera-1", 0xc265000, 15, 115000, 125000, 0 },
#else
	/* redwood (POCO X5 Pro, SM7325) */
	{ "aoss-0", 0xc263000, 0, 115000, 125000, 0 },
	{ "cpu-0-0", 0xc263000, 1, 110000, 125000, SMALL },
	{ "cpu-0-1", 0xc263000, 2, 110000, 125000, SMALL },
	{ "cpu-0-2", 0xc263000, 3, 110000, 125000, SMALL },
	{ "cpu-0-3", 0xc263000, 4, 110000, 125000, SMALL },
	{ "cpuss-0", 0xc263000, 5, 115000, 125000, ALL },
	{ "cpuss-1", 0xc263000, 6, 115000, 125000, ALL },
	{ "cpu-1-0", 0xc263000, 7, 108000, 125000, BIG },
	{ "cpu-1-1", 0xc263000, 8, 108000, 125000, BIG },
	{ "cpu-1-2", 0xc263000, 9, 108000, 125000, BIG },
	{ "cpu-1-3", 0xc263000, 10, 108000, 125000, BIG },
	{ "cpu-1-4", 0xc263000, 11, 108000, 125000, BIG },
	{ "cpu-1-5", 0xc263000, 12, 108000, 125000, BIG },
	{ "cpu-1-6", 0xc263000, 13, 108000, 125000, BIG },
	{ "cpu-1-7", 0xc263000, 14, 108000, 125000, BIG },
	{ "aoss-1", 0xc265000, 0, 115000, 125000, 0 },
	{ "gpuss-0", 0xc265000, 1, 95000, 125000, 0 },
	{ "gpuss-1", 0xc265000, 2, 95000, 125000, 0 },
	{ "nspss-0", 0xc265000, 3, 100000, 125000, 0 },
	{ "nspss-1", 0xc265000, 4, 100000, 125000, 0 },
	{ "video", 0xc265000, 5, 100000, 125000, 0 },
	{ "ddr", 0xc265000, 6, 115000, 125000, 0 },
	{ "mdmss-0", 0xc265000, 7, 95000, 125000, 0 },
	{ "mdmss-1", 0xc265000, 8, 95000, 125000, 0 },
	{ "mdmss-2", 0xc265000, 9, 95000, 125000, 0 },
	{ "mdmss-3", 0xc265000, 10, 95000, 125000, 0 },
	{ "camera-0", 0xc265000, 11, 100000, 125000, 0 },
#endif
};
#define NSENSORS        ((unsigned)(sizeof(sensors) / sizeof(sensors[0])))
#define REFERENCE_TM0   0xc263000u
#define REFERENCE_TM1   0xc265000u

static vm_offset_t tm_va[2];
static uint32_t tm_pa[2] = { REFERENCE_TM0, REFERENCE_TM1 };
static int32_t tenths[NSENSORS];                /* the last good reading; INT32_MIN: none yet */
static uint32_t poll_ms = 2000, log_seconds = 60, stop_celsius = 105;
static uint64_t last_log, reads;
static int strikes, last_step;
static bool stopping;

static int thermal_max_mc = -273000;
static char thermal_hottest[16] = "";
static char thermal_list[NSENSORS * 16 + 8];
static int thermal_reads_value;

SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_max_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_max_mc, 0,
    "the hottest valid temperature sensor of the SoC, in millidegrees (-273000: none read)");
SYSCTL_STRING(_kern_hakios, OID_AUTO, thermal_hottest, CTLFLAG_RD | CTLFLAG_LOCKED, thermal_hottest, 0, "the name of that sensor");
SYSCTL_STRING(_kern_hakios, OID_AUTO, thermal_sensors, CTLFLAG_RD | CTLFLAG_LOCKED, thermal_list, 0,
    "every sensor and its temperature in degrees: \"cpu-0-0=41.2 gpuss-0=38.0 ...\"");
SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_reads_value, 0, "how many times the sensors were read");

/*
 * The brake and the pressure (see the top of the file). margin_c: how far below a sensor's own "slow down" trip the
 * brake starts (thermal_throttle=, 0: never); step_c: one more row of the frequency table off every so many degrees
 * closer (thermal_step=). The pressure level is OSThermalPressureLevel's iOS numbering (libkern/OSThermalNotification.h).
 */
extern uint32_t hakios_cpufreq_cap(uint32_t domain, uint32_t steps);
extern uint32_t hakios_cpufreq_steps(uint32_t domain);

#define GROUPS                  3
#define HYSTERESIS_TENTHS       20              /* back off a step only 2 degrees cooler than it was taken at */
#define PRESSURE_NOMINAL        0
#define PRESSURE_LIGHT          10
#define PRESSURE_MODERATE       20
#define PRESSURE_HEAVY          30
#define PRESSURE_TRAPPING       40

static uint32_t margin_c = 15, step_c = 3;
static uint32_t group_steps[GROUPS], group_mhz[GROUPS];
static int thermal_pressure_value = PRESSURE_NOMINAL;
static int thermal_headroom_value = 1000;
static char thermal_brake[64] = "";
static int thermal_brakes_value;

SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_pressure, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_pressure_value, 0,
    "the thermal pressure level, as OSThermalPressureLevel on iOS: 0 nominal, 10 light, 20 moderate, 30 heavy, 40 trapping");
SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_headroom, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_headroom_value, 0,
    "degrees from the nearest sensor to its own \"slow down\" trip point (negative: past it)");
SYSCTL_STRING(_kern_hakios, OID_AUTO, thermal_brake, CTLFLAG_RD | CTLFLAG_LOCKED, thermal_brake, 0,
    "each group of cores: rows of its frequency table held back and the MHz it is at: \"0:0/1804 1:2/2054 2:2/2400\"");
SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_brakes, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_brakes_value, 0,
    "how many times the brake has changed a group's frequency");

/* rows to hold back for a headroom (tenths of a degree to the trip): none outside the margin, then one per step_c */
static uint32_t
steps_for(int32_t headroom)
{
	int32_t margin = (int32_t)margin_c * 10;

	if (margin_c == 0 || headroom >= margin) {
		return 0;
	}
	return 1 + (uint32_t)((margin - headroom) / ((int32_t)step_c * 10));
}

/*
 * The level for a headroom. Foundation turns the levels into NSProcessInfo.thermalState (seen on the phone: 30 is
 * "critical"), so: nominal until the brake starts; light ("fair") in the first half of the margin; moderate
 * ("serious") in the second half; heavy ("critical") past the vendor's trip point; trapping 10 degrees past it.
 */
static int
pressure_for(int32_t headroom)
{
	int32_t margin = (int32_t)margin_c * 10;

	if (margin_c == 0) {
		margin = 150;                   /* no brake, but the level still means something to iOS */
	}
	return headroom < -100 ? PRESSURE_TRAPPING : headroom < 0 ? PRESSURE_HEAVY : headroom < margin / 2 ? PRESSURE_MODERATE :
	       headroom < margin ? PRESSURE_LIGHT : PRESSURE_NOMINAL;
}

static void
brake(void)
{
	int32_t room[GROUPS] = { INT32_MAX, INT32_MAX, INT32_MAX }, worst = INT32_MAX;
	bool changed = false;
	char text[sizeof(thermal_brake)];

	for (unsigned i = 0; i < NSENSORS; i++) {
		int32_t headroom;

		if (tenths[i] == INT32_MIN) {
			continue;
		}
		headroom = sensors[i].slow_mc / 100 - tenths[i];
		worst = MIN(worst, headroom);
		for (unsigned g = 0; g < GROUPS; g++) {
			if (sensors[i].groups & (1u << g)) {
				room[g] = MIN(room[g], headroom);
			}
		}
	}
	if (worst == INT32_MAX) {
		return;
	}
	text[0] = 0;
	for (unsigned g = 0; g < GROUPS; g++) {
		uint32_t most = hakios_cpufreq_steps(g), wanted;

		if (most == 0) {
			continue;                       /* a group cpufreq did not raise: nothing to hold back */
		}
		wanted = room[g] == INT32_MAX ? 0 : steps_for(room[g]);
		if (wanted < group_steps[g]) {
			/* cooler: one step back at a time, and only once it is HYSTERESIS degrees past where that step began */
			wanted = steps_for(room[g] - HYSTERESIS_TENTHS) < group_steps[g] ? group_steps[g] - 1 : group_steps[g];
		}
		wanted = MIN(wanted, most);
		if (wanted != group_steps[g] || group_mhz[g] == 0) {
			uint32_t mhz = hakios_cpufreq_cap(g, wanted);

			if (wanted != group_steps[g]) {
				say("thermal: group %u: %d.%d C from its trip point, %u of %u rows held back: %u MHz\n", g, room[g] / 10,
				    (room[g] < 0 ? -room[g] : room[g]) % 10, wanted, most, mhz);
				thermal_brakes_value++;
				changed = true;
			}
			group_steps[g] = wanted;
			group_mhz[g] = mhz;
		}
		snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%u:%u/%u", text[0] ? " " : "", g, group_steps[g], group_mhz[g]);
	}
	strlcpy(thermal_brake, text, sizeof(thermal_brake));

	int level = pressure_for(worst);

	if (level < thermal_pressure_value) {
		/* (the same: down a level at a time, HYSTERESIS degrees cooler than where it went up) */
		level = pressure_for(worst - HYSTERESIS_TENTHS) < thermal_pressure_value ? thermal_pressure_value - 10 :
		    thermal_pressure_value;
	}
	thermal_headroom_value = worst / 10;
	if (level != thermal_pressure_value || changed) {
		if (level != thermal_pressure_value) {
			say("thermal: pressure %d -> %d (the nearest sensor %d.%d C from its trip point)\n", thermal_pressure_value, level,
			    worst / 10, (worst < 0 ? -worst : worst) % 10);
		}
		thermal_pressure_value = level;
	}
}

/* For other drivers (a GPU or modem that wants to back off): the hottest temperature, in millidegrees; -273000: not read. */
int
hakios_thermal_max_mc(void)
{
	return thermal_max_mc;
}

static thread_call_t call;

static void
text_of(char *out, size_t size, int32_t tenth)
{
	int whole = tenth / 10, frac = tenth % 10;

	snprintf(out, size, "%s%d.%d", (tenth < 0 && whole == 0) ? "-" : "", whole, frac < 0 ? -frac : frac);
}

static void
look(void)
{
	int32_t hottest = INT32_MIN;
	unsigned hot = 0, good = 0;
	char list[sizeof(thermal_list)], one[16];

	list[0] = 0;
	for (unsigned i = 0; i < NSENSORS; i++) {
		uint32_t status = *(volatile uint32_t *)(tm_va[sensors[i].tm == REFERENCE_TM0 ? 0 : 1] + TM_STATUS(sensors[i].number));
		int32_t value = (int32_t)(status << 20) >> 20;

		if (!(status & TM_VALID) || value < TEMP_MIN_TENTHS || value > TEMP_MAX_TENTHS) {
			continue;
		}
		tenths[i] = value;
		good++;
		if (value > hottest) {
			hottest = value;
			hot = i;
		}
		text_of(one, sizeof(one), value);
		if (strlen(list) + strlen(sensors[i].name) + strlen(one) + 3 < sizeof(list)) {
			strlcat(list, sensors[i].name, sizeof(list));
			strlcat(list, "=", sizeof(list));
			strlcat(list, one, sizeof(list));
			strlcat(list, " ", sizeof(list));
		}
	}
	reads++;
	thermal_reads_value = (int)reads;
	if (good == 0) {
		return;
	}
	strlcpy(thermal_list, list, sizeof(thermal_list));
	strlcpy(thermal_hottest, sensors[hot].name, sizeof(thermal_hottest));
	thermal_max_mc = hottest * 100;

	uint64_t now = mach_absolute_time(), seconds;
	int step = hottest >= 600 ? hottest / 50 : 0;           /* a step each 5 degrees from 60 */

	absolutetime_to_nanoseconds(now - last_log, &seconds);
	seconds /= NSEC_PER_SEC;
	if (step > last_step || last_log == 0 || (log_seconds != 0 && seconds >= log_seconds)) {
		text_of(one, sizeof(one), hottest);
		say("thermal: %u of %u sensors read; hottest %s at %s C; %s\n", good, NSENSORS, sensors[hot].name, one, thermal_list);
		last_log = now;
	}
	last_step = step;
	brake();

	if (stop_celsius != 0 && hottest >= (int32_t)stop_celsius * 10) {
		strikes++;
	} else {
		strikes = 0;
	}
	if (strikes >= 3 && !stopping) {
		stopping = true;
		text_of(one, sizeof(one), hottest);
		say("\n--- thermal: %s reads %s C, at or above thermal_stop=%u three times in a row: back to the loader ---\n",
		    sensors[hot].name, one, stop_celsius);
		hakios_var_save_on_return();
		hakios_adreno_stop();
		hakios_wifi_stop();
		hakios_adsp_stop();
		loader_return();
	}
}

static void
tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	look();
	clock_interval_to_deadline(poll_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

__startup_func
static void
thermal_init(void)
{
	uint32_t enabled = 0, value;

	if (!PE_parse_boot_argn("thermal", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned i = 0; i < NSENSORS; i++) {
		tenths[i] = INT32_MIN;
	}
	(void)PE_parse_boot_argn("thermal_tm0", &tm_pa[0], sizeof(tm_pa[0]));
	(void)PE_parse_boot_argn("thermal_tm1", &tm_pa[1], sizeof(tm_pa[1]));
	if (PE_parse_boot_argn("thermal_poll", &value, sizeof(value)) && value >= 200 && value <= 60000) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("thermal_log", &value, sizeof(value)) && value <= 86400) {
		log_seconds = value;
	}
	if (PE_parse_boot_argn("thermal_stop", &value, sizeof(value)) && value <= 160) {
		stop_celsius = value;
	}
	if (PE_parse_boot_argn("thermal_throttle", &value, sizeof(value)) && value <= 100) {
		margin_c = value;
	}
	if (PE_parse_boot_argn("thermal_step", &value, sizeof(value)) && value >= 1 && value <= 20) {
		step_c = value;
	}
	for (unsigned b = 0; b < 2; b++) {
		tm_va[b] = ml_io_map(tm_pa[b], TM_SIZE);
		if (tm_va[b] == 0) {
			say("thermal: could not map the sensors' block at %#x; off\n", tm_pa[b]);
			return;
		}
	}
	say("thermal: %u sensors in two blocks (%#x, %#x), every %u ms; brake from %u C below each trip point, a row every %u C%s; "
	    "stop at %u C%s\n", NSENSORS, tm_pa[0], tm_pa[1], poll_ms, margin_c, step_c, margin_c ? "" : " (off)", stop_celsius,
	    stop_celsius ? "" : " (off)");
	call = thread_call_allocate(tick, NULL);
	tick(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, thermal_init);
