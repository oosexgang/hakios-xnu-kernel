/*
 * redwood: symbols that iOS 26 (xnu-12377) moved out of the open source into the per-SoC closed
 * KernelSupport library (lib<MACHINE>.os.<CONFIG>.a).  A non-Apple board (BCM2837) has no such
 * library, so provide minimal definitions here.  Values are either set later by the kernel
 * (master_cpu) or chosen as the safe default for a development board.
 */
#include <mach/mach_types.h>
#include <kern/kern_types.h>

/* The boot processor's CPU number; arm_init() assigns it from ml_get_boot_cpu_number(). */
int master_cpu = -1;

/* Not production-fused: this is a bring-up/development board. */
boolean_t
ml_device_is_prod_fused(void)
{
	return FALSE;
}

/* Jetsam critical-pressure thresholds; memorystatus_init() recomputes both at boot. */
unsigned int memorystatus_available_pages_critical = 0;
unsigned int memorystatus_available_pages_critical_base = 0;
