/*
 * How long each CPU has been idle, from the scheduler's own bookkeeping (recount: every idle entry
 * and exit is stamped), and which cluster a CPU is in (MPIDR Aff2).
 */
#include <kern/processor.h>
#include <kern/recount.h>
#include <machine/machine_routines.h>

uint64_t hakios_cpu_idle_mach(int cpu);
uint64_t
hakios_cpu_idle_mach(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	struct recount_usage usage;
	uint64_t idle = 0;

	if (p == PROCESSOR_NULL) {
		return 0;
	}
	recount_processor_usage(&p->pr_recount, &usage, &idle);
	return idle;
}

int hakios_cpu_cluster(int cpu);
int
hakios_cpu_cluster(int cpu)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	if (topology == NULL || cpu < 0 || (unsigned)cpu >= topology->num_cpus) {
		return -1;
	}
	return (int)((topology->cpus[cpu].phys_id >> 16) & 0xff);
}
