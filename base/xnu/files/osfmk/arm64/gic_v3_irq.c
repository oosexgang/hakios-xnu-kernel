/*
 * Minimal GICv3 support for generic (non-Apple) ARMv8 boards.
 *
 * Open-source XNU has no interrupt controller driver: on Apple hardware that
 * is a kext (AppleInterruptController), and the timer arrives as FIQ. Here
 * everything is a Group 1 IRQ, which is what an EL1 guest gets from a stock
 * hypervisor or from QEMU's virt machine:
 *
 *   - the CPU interface is driven through the ICC_*_EL1 system registers
 *   - the EL1 virtual timer PPI is routed to the kernel's rtclock
 *   - with more than one processor (psci_smp.c), two software-generated
 *     interrupts carry the kernel's signals between them: cpu_signal() and
 *     the order to stop when the system goes back to the boot loader
 *   - a shared peripheral interrupt (SPI, numbers 32 to 1019) is a driver's once it
 *     is attached (gic_v3_spi_attach()): configured, routed to the processor that
 *     attaches it, enabled, and its handler called in interrupt context for each
 *     one that arrives, before it is ended
 *   - any other interrupt that fires is masked and counted, until a real
 *     driver registers a handler for it
 *
 * Every processor has its own CPU interface, redistributor frame and timer,
 * and sets them up for itself (gic_v3_irq_cpu_init(), from PE_init_cpu()).
 *
 * Registers come from the device tree node /arm-io/gic:
 *   reg = <distributor offset, size, redistributor offset, size>
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <os/atomic_private.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>

#if defined(GENERIC_GIC_V3_IRQ)

#define GICD_CTLR               0x0000
#define GICD_CTLR_ENABLE_G1NS   (1u << 1)
#define GICD_CTLR_ARE_NS        (1u << 4)
#define GICD_CTLR_RWP           (1u << 31)
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + (n))
#define GICD_ICFGR(n)           (0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)        (0x0d00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))

#define GICR_FRAME_SIZE         0x20000     /* RD_base + SGI_base per CPU */
#define GICR_TYPER              0x0008
#define GICR_TYPER_LAST         (1u << 4)
#define GICR_WAKER              0x0014
#define GICR_WAKER_PROCSLEEP    (1u << 1)
#define GICR_WAKER_CHILDASLEEP  (1u << 2)
#define GICR_SGI_BASE           0x10000
#define GICR_IGROUPR0           (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0         (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0         (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0           (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0         (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR(n)      (GICR_SGI_BASE + 0x0400 + (n))
#define GICR_IGRPMODR0          (GICR_SGI_BASE + 0x0d00)

#define GIC_SGI_SIGNAL          0           /* cpu_signal(): another processor wants this one's attention */
#define GIC_SGI_STOP            1           /* psci_smp_stop_here(): the system is going back to the boot loader */
#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */
#define GIC_INTID_SPI_MIN       32
#define GIC_INTID_SPURIOUS_MIN  1020
#define GIC_SPI_MOST            48          /* attached at a time */
#define GIC_PRIORITY_DEFAULT    0xa0

static vm_offset_t gicd_base, gicr_base;
static vm_size_t   gicr_size;
static vm_offset_t gicr_cpu_base[MAX_CPUS]; /* each processor's redistributor frame */

/*
 * Acknowledged, not yet EOI'd; 0: none. The signal between processors is interrupt 0 and reads
 * as 0 from ICC_IAR1_EL1, hence the mark: without it a core switched off in the middle of that
 * signal (which is where every core but one is during a panic) kept it active, and with the
 * active priority still set in its CPU interface the next kernel on that core saw no interrupt
 * at all.
 */
#define GIC_IAR_TAKEN           (1ULL << 63)
static uint64_t gic_active_iar[MAX_CPUS];
uint64_t        gic_unhandled_count;
uint32_t        gic_last_unhandled;

/*
 * The attached SPIs. A slot is filled before its interrupt is enabled and emptied after it has
 * been disabled and no handler of it runs any more (busy), so the handler path reads the table
 * without a lock.
 */
struct gic_spi {
	uint32_t intid;                     /* 0: a free slot */
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	uint32_t busy;                      /* handlers of it running now */
	uint64_t count;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);

void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
void gic_v3_irq_cpu_shutdown(void);
void gic_v3_irq_signal(uint32_t phys_id);
void gic_v3_irq_signal_stop(uint32_t phys_id);

static inline uint32_t
gic_read32(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static void
gic_mask(uint32_t intid)
{
	if (intid < 32) {
		gic_write32(gicr_cpu_base[cpu_number()] + GICR_ICENABLER0, 1u << intid);
	} else {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A software-generated interrupt for the processor with that MPIDR (its two
 * low affinity fields, as in the device tree's "reg").
 * ICC_SGI1R_EL1: Aff1 in [23:16], the interrupt in [27:24], and in [15:0] a
 * bit for every Aff0 addressed -- which is why Aff0 has to be below 16.
 */
static void
gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	uint64_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff;

	__builtin_arm_wsr64("ICC_SGI1R_EL1", ((uint64_t)sgi << 24) | (aff1 << 16) | (1ULL << (aff0 & 15)));
	__builtin_arm_isb(ISB_SY);
}

/* The platform's half of cpu_signal(): interrupt that processor (GenericARMCPU::signalCPU()). */
void
gic_v3_irq_signal(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_SIGNAL);
}

void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_STOP);
}

/* An SPI that a driver has attached: its handler called. false: nobody has it. */
static bool
gic_spi_handle(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		struct gic_spi *spi = &gic_spi[i];

		if (os_atomic_load(&spi->intid, acquire) == intid) {
			os_atomic_inc(&spi->busy, acquire);
			if (os_atomic_load(&spi->intid, relaxed) == intid) {
				spi->count++;
				spi->handler(spi->context, intid);
			}
			os_atomic_dec(&spi->busy, release);
			return true;
		}
	}
	return false;
}

static void
gic_distributor_settle(void)
{
	while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
		/* wait for the change to take effect */
	}
}

void
gic_v3_spi_mask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		gic_distributor_settle();
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A driver takes an SPI: edge- or level-triggered, Group 1, the default priority, routed to the
 * processor this runs on, and enabled. The handler runs in interrupt context, with interrupts off,
 * before the interrupt is ended; for a level-triggered one it has to quiet the source or mask the
 * interrupt (gic_v3_spi_mask()). Thread context.
 */
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	struct gic_spi *slot = NULL;
	uint32_t config;

	if (gicd_base == 0 || intid < GIC_INTID_SPI_MIN || intid >= GIC_INTID_SPURIOUS_MIN || handler == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			simple_unlock(&gic_spi_lock);
			return KERN_NAME_EXISTS;
		}
		if (slot == NULL && gic_spi[i].intid == 0 && gic_spi[i].busy == 0) {
			slot = &gic_spi[i];
		}
	}
	if (slot == NULL) {
		simple_unlock(&gic_spi_lock);
		return KERN_RESOURCE_SHORTAGE;
	}
	slot->handler = handler;
	slot->context = context;
	slot->count = 0;
	os_atomic_store(&slot->intid, intid, release);
	simple_unlock(&gic_spi_lock);

	gic_v3_spi_mask(intid);
	gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GICD_ICACTIVER(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GICD_IGROUPR(intid / 32), gic_read32(gicd_base + GICD_IGROUPR(intid / 32)) | 1u << (intid % 32));
	gic_write32(gicd_base + GICD_IGRPMODR(intid / 32), gic_read32(gicd_base + GICD_IGRPMODR(intid / 32)) & ~(1u << (intid % 32)));
	*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(intid)) = GIC_PRIORITY_DEFAULT;
	config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
	gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
	/* GICD_IROUTER: Aff3 in [39:32], Aff2.Aff1.Aff0 in [23:0]; bit 31 (any processor) clear */
	*(volatile uint64_t *)(gicd_base + GICD_IROUTER(intid)) = (mpidr & 0xff00000000ull) | (mpidr & 0xffffff);
	gic_distributor_settle();
	gic_v3_spi_unmask(intid);
	return KERN_SUCCESS;
}

/* ... and gives it back: disabled, nothing pending, and no handler of it running when this returns. */
void
gic_v3_spi_detach(uint32_t intid)
{
	gic_v3_spi_mask(intid);
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	}
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			os_atomic_store(&gic_spi[i].intid, 0, release);
			while (os_atomic_load(&gic_spi[i].busy, acquire) != 0) {
				/* a handler of it on another processor is finishing */
			}
		}
	}
}

/* How many have arrived since it was attached. */
uint64_t
gic_v3_spi_count(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			return gic_spi[i].count;
		}
	}
	return 0;
}

static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	int cpu = cpu_number();

	for (;;) {
		uint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
		uint32_t intid = (uint32_t)(iar & 0xffffff);

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;
		}
		gic_active_iar[cpu] = iar | GIC_IAR_TAKEN;

		if (intid == GIC_INTID_VTIMER) {
			/* same sequence as the FIQ path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1; /* Large */
			rtclock_intr(TRUE);
		} else if (intid == GIC_SGI_SIGNAL) {
			cpu_signal_handler();
		} else if (intid == GIC_SGI_STOP) {
			/* comes back only if nobody has in fact asked for that */
			psci_smp_stop_here();
		} else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
			/* a driver's */
		} else {
			/* nobody owns this interrupt yet: keep it from storming */
			gic_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
		__builtin_arm_isb(ISB_SY);
		gic_active_iar[cpu] = 0;
	}
}

/*
 * Leave the controller in a state another environment can take over from:
 * finish any interrupt that this processor acknowledged but did not complete.
 * Used when control goes back to the boot loader (see loader_return.c), and
 * by a processor that is about to be switched off for that (psci_smp.c).
 */
void
gic_v3_irq_quiesce(void)
{
	int cpu;

	if (gicd_base == 0) {
		return;         /* as early as this there is no interrupt, nor a cpu number to ask for */
	}
	cpu = cpu_number();
	/* (what drivers had of the shared interrupts goes with this kernel: none of them may fire under the loader) */
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = gic_spi[i].intid;

		if (intid != 0) {
			gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		}
	}
	if (gic_active_iar[cpu] != 0) {
		__builtin_arm_wsr64("ICC_EOIR1_EL1", gic_active_iar[cpu] & ~GIC_IAR_TAKEN);
		__builtin_arm_isb(ISB_SY);
		gic_active_iar[cpu] = 0;
	}
}

/*
 * This processor is leaving for good: the firmware is about to be asked to switch it off
 * (psci_smp_cpu_off(), and the boot processor in the loader's stub when the kernel has a core of
 * its own). Nothing may be left that could interrupt it. The redistributor outlives the core, and
 * so does what is pending in it: the "stop" signal that reached a core whose interrupts were off
 * (every core is in that state while another one panics), or its timer. A core asked to go with
 * an interrupt pending and the CPU interface still passing it on does not go cleanly, and the
 * next kernel that is started on it hangs.
 */
void
gic_v3_irq_cpu_shutdown(void)
{
	vm_offset_t gicr;

	if (gicd_base == 0) {
		return;
	}
	gic_v3_irq_quiesce();
	gicr = gicr_cpu_base[cpu_number()];
	if (gicr != 0) {
		gic_write32(gicr + GICR_ICENABLER0, 0xffffffffu);
		gic_write32(gicr + GICR_ICPENDR0, 0xffffffffu);
		gic_write32(gicr + GICR_ICACTIVER0, 0xffffffffu);
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
	__builtin_arm_isb(ISB_SY);
}

static vm_offset_t
gic_find_redistributor(void)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	/* GICR_TYPER[63:32] packs Aff3.Aff2.Aff1.Aff0 */
	uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));

	for (vm_offset_t frame = gicr_base; frame + GICR_FRAME_SIZE <= gicr_base + gicr_size; frame += GICR_FRAME_SIZE) {
		uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);

		if ((uint32_t)(typer >> 32) == affinity) {
			return frame;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
	}
	panic("gic: no redistributor for MPIDR 0x%llx", mpidr);
}

/* Per-CPU bring-up: every processor calls this for itself, the boot processor first. */
void
gic_v3_irq_cpu_init(void)
{
	int cpu = cpu_number();
	vm_offset_t gicr;
	uint32_t mine = 1u << GIC_INTID_VTIMER;

	if (gicd_base == 0) {
		DTEntry entry;
		void const *prop;
		unsigned int prop_size;

		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
		    prop_size < 4 * sizeof(uint64_t)) {
			panic("gic: no usable /arm-io/gic node in the device tree");
		}
		uint64_t const *reg = prop;
		vm_offset_t soc_base = pe_arm_get_soc_base_phys();

		simple_lock_init(&gic_spi_lock, 0);
		gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
		gicr_base = ml_io_map(soc_base + reg[2], reg[3]);
		gicr_size = reg[3];
		if (gicd_base == 0 || gicr_base == 0) {
			panic("gic: cannot map the distributor/redistributor");
		}
	} else if (gicr_cpu_base[cpu] != 0) {
		return;
	}

	/* System-register CPU interface */
	__builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
	__builtin_arm_isb(ISB_SY);

	gicr = gicr_cpu_base[cpu] = gic_find_redistributor();

	uint32_t waker = gic_read32(gicr + GICR_WAKER);
	if (waker & (GICR_WAKER_PROCSLEEP | GICR_WAKER_CHILDASLEEP)) {
		gic_write32(gicr + GICR_WAKER, waker & ~GICR_WAKER_PROCSLEEP);
		while (gic_read32(gicr + GICR_WAKER) & GICR_WAKER_CHILDASLEEP) {
			/* spin until the redistributor is awake */
		}
	}

	/*
	 * The interrupts between processors, where there is more than one. A core that an earlier
	 * kernel switched off in the middle of one (a return to the boot loader, started again
	 * since) may still have it marked active, and would then never see another.
	 */
	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicr + GICR_ICACTIVER0, sgis);
		gic_write32(gicr + GICR_ICPENDR0, sgis);        /* ... or one that it never took */
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
		mine |= sgis;
	}

	/* Virtual timer PPI (and those): Group 1 (IRQ), default priority, enabled */
	gic_write32(gicr + GICR_IGROUPR0, gic_read32(gicr + GICR_IGROUPR0) | mine);
	gic_write32(gicr + GICR_IGRPMODR0, gic_read32(gicr + GICR_IGRPMODR0) & ~mine);
	*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicr + GICR_ISENABLER0, mine);

	/* Distributor: affinity routing, Group 1 enabled (normally already so) */
	uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
	if ((ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) != (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) {
		gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
		while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
			/* wait for the write to take effect */
		}
	}

	/*
	 * CPU interface: accept all priorities, no preemption grouping, Group 1 on. And no priority
	 * active: a core that the firmware switches off and on again need not lose that, and a
	 * kernel that left in the middle of an interrupt without ending it would leave this one
	 * waiting for an end that never comes.
	 */
	__builtin_arm_wsr64("ICC_AP1R0_EL1", 0);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	kprintf("gic: v3, distributor %p, cpu %d's redistributor %p, virtual timer on PPI %d\n",
	    (void *)gicd_base, cpu, (void *)gicr, GIC_INTID_VTIMER);
}

#endif /* GENERIC_GIC_V3_IRQ */
