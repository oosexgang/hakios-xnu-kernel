/*
 * The phone's heat as iOS hears of it. On an iPhone thermalmonitord reads Apple's sensors and posts the thermal
 * pressure level: notify(3) name "com.apple.system.thermalpressurelevel" (kOSThermalNotificationPressureLevelName),
 * whose state is an OSThermalPressureLevel (libkern/OSThermalNotification.h, iOS numbering: 0 nominal, 10 light,
 * 20 moderate, 30 heavy, 40 trapping, 50 sleeping). Foundation's NSProcessInfo.thermalState and its
 * NSProcessInfoThermalStateDidChangeNotification, and everything that reads OSThermalPressureLevel, follow that name.
 * thermalmonitord does not run here (it wants Apple's own sensor and CLPC drivers), so nobody posts it and it stays 0.
 * The kernel works the level out from the SoC's sensors (drivers/power/hakios.thermal.c: kern.hakios.thermal_pressure);
 * this posts it.
 *
 *   thermal_notify            the job (com.hakios.thermal.plist): every 2 s, the kernel's level; when it changes, the
 *                             name's state set to it and the name posted. Ends at once if the kernel has no level
 *                             (thermal=0, or a kernel without the driver).
 *   thermal_notify status     what everybody sees: the kernel's numbers, the name's state, NSProcessInfo.thermalState
 *   thermal_notify load S [N] N threads (default: one per core) spinning for S seconds, the status every 5 s: heat
 *                             to watch the brake by
 */
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define PRESSURE_NAME   "com.apple.system.thermalpressurelevel"

static const struct {
	const char *name;
	uint64_t state;
} battery_names[] = {
	{ "com.apple.thermalmonitor.ageAwareMitigationState", 2 },
	{ "com.apple.system.batteryHealth.UPOAware", 0x4e4f },           /* "NO" */
	{ "com.apple.system.batteryHealth.p0Threshold", 100 },
};

static int
number(const char *name, int otherwise)
{
	int value = otherwise;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : otherwise;
}

static void
text(const char *name, char *out, size_t size)
{
	out[0] = 0;
	if (sysctlbyname(name, out, &size, NULL, 0) != 0) {
		strlcpy(out, "-", 4);
	}
}

static long
thermal_state(void)
{
	Class class = objc_getClass("NSProcessInfo");
	id info;

	if (class == NULL) {
		return -1;
	}
	info = ((id (*)(Class, SEL))objc_msgSend)(class, sel_registerName("processInfo"));
	return info ? ((long (*)(id, SEL))objc_msgSend)(info, sel_registerName("thermalState")) : -1;
}

static void
status(void)
{
	static const char *states[] = { "nominal", "fair", "serious", "critical" };
	char hottest[32], brake[96];
	uint64_t memsize = 0, state = 0;
	size_t size = sizeof(memsize);
	int token;
	long ts;

	sysctlbyname("hw.memsize", &memsize, &size, NULL, 0);
	text("kern.hakios.thermal_hottest", hottest, sizeof(hottest));
	text("kern.hakios.thermal_brake", brake, sizeof(brake));
	if (notify_register_check(PRESSURE_NAME, &token) == NOTIFY_STATUS_OK) {
		notify_get_state(token, &state);
		notify_cancel(token);
	}
	ts = thermal_state();
	printf("thermal_notify: hw.memsize %llu MB; hottest %s at %d mC, headroom %d C, kernel pressure %d; brake [%s]; "
	    "%s state %llu; NSProcessInfo.thermalState %ld (%s)\n", memsize >> 20, hottest,
	    number("kern.hakios.thermal_max_mc", -273000), number("kern.hakios.thermal_headroom", 0),
	    number("kern.hakios.thermal_pressure", -1), brake, PRESSURE_NAME, state, ts,
	    ts >= 0 && ts < 4 ? states[ts] : "?");
	fflush(stdout);
}

static volatile int spinning = 1;

static void *
spin(void *arg)
{
	volatile uint64_t x = (uintptr_t)arg;

	while (spinning) {
		for (int i = 0; i < 1000000; i++) {
			x = x * 6364136223846793005ULL + 1442695040888963407ULL;
		}
	}
	return NULL;
}

static void
load(int seconds, int threads)
{
	pthread_t thread[64];

	if (threads <= 0) {
		threads = number("hw.ncpu", 8);
	}
	threads = threads > 64 ? 64 : threads;
	printf("thermal_notify: %d threads spinning for %d s\n", threads, seconds);
	for (int i = 0; i < threads; i++) {
		pthread_create(&thread[i], NULL, spin, (void *)(uintptr_t)i);
	}
	for (int t = 0; t < seconds; t += 5) {
		sleep(5);
		status();
	}
	spinning = 0;
	for (int i = 0; i < threads; i++) {
		pthread_join(thread[i], NULL);
	}
	printf("thermal_notify: load done\n");
	status();
}

int
main(int argc, char **argv)
{
	int token, posted = -1;

	if (argc > 1 && strcmp(argv[1], "status") == 0) {
		status();
		return 0;
	}
	if (argc > 2 && strcmp(argv[1], "load") == 0) {
		load(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0);
		return 0;
	}
	/*
	 * thermalmonitord's battery names, which Settings > Battery > Battery Health needs before it shows the battery's
	 * maximum capacity:
	 *  - the age-aware mitigation state, which BatteryUsageUI's getManagementState wants in 1..3 (with nothing posted
	 *    it is 0 and the page keeps its spinner). Its Peak Performance text goes by it too: 1 "performance management has
	 *    been applied", 2 "normal peak performance", 3 turned off by the user, 4 dynamic. 2: nothing here holds the CPU
	 *    back for the battery's age (zeus log 045053; log 004744 showed 1's text).
	 *  - batteryHealth.UPOAware: whether the phone steps its peak performance down after unexpected power-offs. "NO"
	 *    (powerd reads 'N' 'O' as 0x4e4f, and "YES" as 0x594553); unset, powerd cannot tell the battery's health.
	 *  - batteryHealth.p0Threshold, which powerd holds the battery's resistance (the kernel's BatteryData WeightedRa,
	 *    drivers/power/hakios.battery.cpp: 1, nothing measured) against; unset, again powerd cannot tell.
	 * With them powerd keeps Battery Service State from what the gauge says alone: zeus's 79% gets "service
	 * recommended", as an iPhone's under 80% would. The tokens stay open: the names keep their state while they are.
	 */
	for (unsigned i = 0; i < sizeof(battery_names) / sizeof(battery_names[0]); i++) {
		int t;

		if (notify_register_check(battery_names[i].name, &t) == NOTIFY_STATUS_OK) {
			uint32_t set = notify_set_state(t, battery_names[i].state), post = notify_post(battery_names[i].name);

			printf("thermal_notify: %s %#llx (set %u, post %u)\n", battery_names[i].name, battery_names[i].state, set, post);
		}
	}
	fflush(stdout);
	if (number("kern.hakios.thermal_pressure", -1) < 0) {
		printf("thermal_notify: the kernel keeps no thermal pressure level (thermal=0?): nothing to post\n");
		fflush(stdout);
		for (;;) {
			pause();        /* (holding the battery names' state) */
		}
	}
	if (notify_register_check(PRESSURE_NAME, &token) != NOTIFY_STATUS_OK) {
		printf("thermal_notify: could not register %s\n", PRESSURE_NAME);
		return 1;
	}
	for (;;) {
		int level = number("kern.hakios.thermal_pressure", 0);

		if (level != posted) {
			uint32_t set = notify_set_state(token, (uint64_t)level), post = notify_post(PRESSURE_NAME);

			printf("thermal_notify: thermal pressure %d -> %d (%s: set %u, post %u)\n", posted < 0 ? 0 : posted, level,
			    PRESSURE_NAME, set, post);
			fflush(stdout);
			posted = level;
		}
		sleep(2);
	}
}
