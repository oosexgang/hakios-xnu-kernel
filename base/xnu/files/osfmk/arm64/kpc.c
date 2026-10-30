/*
 * Machine-dependent kpc (kernel performance counters) for non-Apple ARMv8
 * cores. Apple does not publish its arm64 kpc.c. This version reports a PMU
 * with no counters, so the kpc/kperf machinery stays linked but inert.
 */
#include <kern/kpc.h>
#include <kern/kern_types.h>
#include <mach/kern_return.h>
#include <sys/errno.h>

int kpc_pc_capture = 0;

void
kpc_arch_init(void)
{
}

uint32_t
kpc_get_classes(void)
{
	return 0;
}

int
kpc_get_pmu_version(void)
{
	return KPC_PMU_ERROR;
}

int
kpc_set_sw_inc(uint32_t mask __unused)
{
	return ENOTSUP;
}

void
kpc_idle(void)
{
}

void
kpc_idle_exit(void)
{
}

int
kpc_get_all_cpus_counters(uint32_t classes __unused, int *curcpu __unused, uint64_t *buf __unused)
{
	return 0;
}

int
kpc_get_fixed_counters(uint64_t *counterv __unused)
{
	return 0;
}

int
kpc_get_configurable_counters(uint64_t *counterv __unused, uint64_t pmc_mask __unused)
{
	return 0;
}

boolean_t
kpc_is_running_fixed(void)
{
	return FALSE;
}

boolean_t
kpc_is_running_configurable(uint64_t pmc_mask __unused)
{
	return FALSE;
}

uint32_t
kpc_fixed_count(void)
{
	return 0;
}

uint32_t
kpc_configurable_count(void)
{
	return 0;
}

uint32_t
kpc_fixed_config_count(void)
{
	return 0;
}

uint32_t
kpc_configurable_config_count(uint64_t pmc_mask __unused)
{
	return 0;
}

uint32_t
kpc_rawpmu_config_count(void)
{
	return 0;
}

int
kpc_get_fixed_config(kpc_config_t *configv __unused)
{
	return 0;
}

int
kpc_get_configurable_config(kpc_config_t *configv __unused, uint64_t pmc_mask __unused)
{
	return 0;
}

int
kpc_get_rawpmu_config(kpc_config_t *configv __unused)
{
	return 0;
}

uint64_t
kpc_fixed_max(void)
{
	return 0;
}

uint64_t
kpc_configurable_max(void)
{
	return 0;
}

int
kpc_set_config_arch(struct kpc_config_remote *mp_config __unused)
{
	return ENOTSUP;
}

int
kpc_set_period_arch(struct kpc_config_remote *mp_config __unused)
{
	return ENOTSUP;
}

int
kpc_set_running_arch(struct kpc_running_remote *mp_config __unused)
{
	return ENOTSUP;
}
