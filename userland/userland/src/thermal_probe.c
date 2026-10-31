/*
 * thermal_probe: a root of its own for a short run on the phone that checks the kernel's thermal service
 * and lists the audio DSP's services, with no iOS needed (XL_RAMDISK=userland/thermal_probe, as with hello).
 * Every ten seconds for a minute and a half it prints kern.redwood.thermal and, once, the DSP's QMI
 * services (kern.redwood.adsp_services); between prints it keeps one core busy, so the governor has heat
 * to see. Then it waits for the kernel's run to end (loader_return_after / XL_SECS).
 */
#include "mini.h"

static void
show(const char *name)
{
	static char text[8192];
	size_t len = sizeof(text) - 1;

	if (sysctlbyname(name, text, &len, NULL, 0) != 0) {
		printf("probe: %s: not in this kernel\n", name);
		return;
	}
	text[len < sizeof(text) ? len : sizeof(text) - 1] = 0;
	printf("probe: ---- %s ----\n%s", name, text);
}

/* about `ms` of work on this core: a little heat for the sensors */
static void
busy(unsigned ms)
{
	struct timeval a, b;
	volatile unsigned long sink = 0;

	gettimeofday(&a, NULL);
	do {
		for (unsigned i = 0; i < 1000000; i++) {
			sink += i * i;
		}
		gettimeofday(&b, NULL);
	} while ((unsigned long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_usec - a.tv_usec) / 1000) < ms);
}

int
main(int argc, char **argv, char **envp)
{
	int fd = open("/dev/console", 2 /* O_RDWR */, 0);

	(void)argc; (void)argv; (void)envp;

	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
	}
	printf("probe: thermal and DSP probe, pid %d\n", getpid());
	for (int round = 0; round < 9; round++) {
		show("kern.redwood.thermal");
		if (round == 1 || round == 8) {
			show("kern.redwood.adsp_services");
		}
		busy(9000);
		usleep(1000000);
	}
	printf("probe: done; waiting for the run to end\n");
	for (;;) {
		usleep(1000000);
	}
}
