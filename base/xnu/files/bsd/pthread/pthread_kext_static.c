/*
 * Starts the pthread kernel extension when it is linked statically into the
 * kernel image.
 *
 * com.apple.kec.pthread (open source, libpthread/kern) normally arrives as a
 * prelinked kext whose start routine registers its function table with
 * pthread_shims.c. This configuration has no kext loading at all, so the kext
 * is built as libpthread_kext.a, linked in, and started from here before
 * bsd_init() needs it.
 */
#include <mach/kmod.h>
#include <kern/startup.h>
#include <kern/debug.h>

extern kern_return_t pthread_start(kmod_info_t *ki, void *d);

__startup_func
static void
pthread_kext_static_start(void)
{
	kern_return_t kr = pthread_start(NULL, NULL);
	if (kr != KERN_SUCCESS) {
		panic("static pthread kext failed to start: %d", kr);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pthread_kext_static_start);
