/*
 * thermald: the phone's heat, as iOS hears about it.
 *
 * The kernel's thermal service (bsd/dev/redwood_thermal.c in hakios-xnu-kernel) reads the SoC's
 * sensors, slows the cores when they get hot, and keeps a pressure level in kern.redwood.thermal_pressure:
 * iOS's own scale, 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping. On an iPhone it is
 * thermalmonitord that says how hot the phone is; here nobody would, so apps would never hear of it --
 * ProcessInfo.thermalState would stay "nominal" while the cores are being slowed down. This daemon
 * passes the level on the way iOS passes it: as the state of the Darwin notification
 * com.apple.system.thermalpressurelevel (OSThermalNotification.h, which ProcessInfo reads), posted
 * whenever it changes.
 *
 *   thermald            stay, and pass the level on (the job com.redwood.thermald does this)
 *   thermald show       the kernel's table: every sensor, every group of cores (kern.redwood.thermal)
 *   thermald level      the level now, as a number and a word
 *
 * Without the kernel's service (QEMU, thermal=0) it says so once and ends.
 */
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PRESSURE_NAME   "com.apple.system.thermalpressurelevel"
#define POLL_US         1000000                 /* the kernel decides every 250 ms; apps need not hear faster */

static const char *const level_names[] = { "nominal", "moderate", "heavy", "trapping", "sleeping" };

/* the kernel's level now; -1 when there is no thermal service */
static int
kernel_level(void)
{
	int level = -1, running = 0;
	size_t len = sizeof(running);

	if (sysctlbyname("kern.redwood.thermal_running", &running, &len, NULL, 0) != 0 || !running) {
		return -1;
	}
	len = sizeof(level);
	if (sysctlbyname("kern.redwood.thermal_pressure", &level, &len, NULL, 0) != 0) {
		return -1;
	}
	return level;
}

static int
show(void)
{
	static char text[4096];
	size_t len = sizeof(text);

	if (sysctlbyname("kern.redwood.thermal", text, &len, NULL, 0) != 0) {
		printf("thermald: this kernel has no thermal service\n");
		return 1;
	}
	fputs(text, stdout);
	return 0;
}

/* tell iOS: the level as the notification's state, then the notification itself */
static void
post(int token, int level)
{
	uint32_t status = notify_set_state(token, (uint64_t)level);

	if (status != NOTIFY_STATUS_OK) {
		printf("thermald: notify_set_state: %u\n", status);
	}
	status = notify_post(PRESSURE_NAME);
	if (status != NOTIFY_STATUS_OK) {
		printf("thermald: notify_post: %u\n", status);
	}
}

static int
serve(void)
{
	int token, last = -1, level = kernel_level();

	if (level < 0) {
		printf("thermald: no thermal service in this kernel (thermal=0, or not a phone): nothing to pass on\n");
		return 0;
	}
	if (notify_register_check(PRESSURE_NAME, &token) != NOTIFY_STATUS_OK) {
		printf("thermald: cannot register %s\n", PRESSURE_NAME);
		return 1;
	}
	printf("thermald: passing the kernel's thermal pressure to iOS (%s)\n", PRESSURE_NAME);
	for (;;) {
		level = kernel_level();
		if (level >= 0 && level != last) {
			post(token, level);
			printf("thermald: pressure %s (%d)%s\n", level <= 4 ? level_names[level] : "?", level,
			    level >= 2 ? ": the cores are being slowed down" : "");
			fflush(stdout);
			last = level;
		}
		usleep(POLL_US);
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "show") == 0) {
		return show();
	}
	if (argc > 1 && strcmp(argv[1], "level") == 0) {
		int level = kernel_level();
		printf("%d %s\n", level, level >= 0 && level <= 4 ? level_names[level] : "no thermal service");
		return level < 0;
	}
	return serve();
}
