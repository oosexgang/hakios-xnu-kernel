/*
 * FreeBSD's <machine/pcpu.h> for arm64 keeps the processor's own data in register x18 and reads the
 * running thread through it. Under XNU that register is not ours: both are asked of the layer
 * (src/lx_base.c), which knows one processor's worth of such data and a thread record for every
 * thread that is inside the driver.
 */
#ifndef _MACHINE_PCPU_H_
#define _MACHINE_PCPU_H_

#include <machine/cpu.h>
#include <machine/cpufunc.h>

#define PCPU_MD_FIELDS							\
	u_int	pc_acpi_id;						\
	u_int	pc_midr;						\
	uint64_t pc_clock;						\
	struct pmap *pc_curpmap;					\
	uint64_t pc_mpidr;						\
	char __pad[224]

#ifdef _KERNEL
struct pcpu;
struct thread;
struct pcpu *lx_pcpu(void);
struct thread *lx_curthread(void);

#define get_pcpu()		lx_pcpu()
#define get_curthread()		lx_curthread()
#define curthread		lx_curthread()
#define PCPU_GET(member)	(get_pcpu()->pc_ ## member)
#define PCPU_ADD(member, value)	(get_pcpu()->pc_ ## member += (value))
#define PCPU_PTR(member)	(&get_pcpu()->pc_ ## member)
#define PCPU_SET(member, value)	(get_pcpu()->pc_ ## member = (value))
#define PCPU_GET_MPIDR(pc)	((pc)->pc_mpidr)
#endif

#endif
