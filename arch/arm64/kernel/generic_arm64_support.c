/*
 * Routines the open-source tree references but only defines for Apple SoCs.
 * Needed when building for a generic ARMv8 board without a monitor (PPL).
 */
#include <mach/mach_types.h>
#include <arm/machine_routines.h>
#include <arm/caches_internal.h>
#include <arm/pmap.h>

/* No PPL / secure monitor to switch on. */
void
ml_enable_monitor(void)
{
}

/*
 * On Apple cores the "_nohid" variant skips toggling a HID chicken bit;
 * elsewhere it is simply a forced clean to the point of coherency.
 */
void
CleanPoC_DcacheRegion_Force_nopreempt_nohid(vm_offset_t va, size_t length)
{
	CleanPoC_DcacheRegion_Force(va, length);
}

/*
 * Stock LLVM lowers Mach-O static destructors to __cxa_atexit registrations
 * (Apple's clang emits __mod_term_func entries in kernel mode instead).
 * The kernel never exits, so registration is a no-op.
 */
int __cxa_atexit(void (*func)(void *), void *arg, void *dso);
int
__cxa_atexit(void (*func)(void *) __unused, void *arg __unused, void *dso __unused)
{
	return 0;
}

/*
 * Profile-guided-optimisation runtime hooks, normally supplied by Apple's
 * compiler-rt build for the kernel. Report "no profile data".
 */
uint64_t __llvm_profile_get_size_for_buffer_internal(const void *, const void *, const void *, const void *, const void *, const void *);
int __llvm_profile_write_buffer_internal(char *, const void *, const void *, const void *, const void *, const void *, const void *);

uint64_t
__llvm_profile_get_size_for_buffer_internal(const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
	return 0;
}

int
__llvm_profile_write_buffer_internal(char *buf __unused, const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
	return -1;
}
