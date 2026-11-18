/*
 * The interrupt controller of the generic boards (arch/arm64/kernel/gic_v3_irq.c), compiled as it is, against
 * plain memory for the distributor and the redistributors and a stand-in for the CPU interface's system
 * registers: what the controller hands out on an acknowledge is a queue the test fills, and every write to a
 * system register is noted. No register of the processor is read or written for real: the test runs as an
 * ordinary program (on the host, and under qemu-aarch64).
 *
 *   gic_v3_irq_test SCENARIO        one scenario a process: the code keeps its state in statics
 */
#include <setjmp.h>
#include <stdarg.h>

#include "fakekernel.h"

extern void loader_log_line(const char *line);

/* ---- the processor's system registers ---------------------------------------------------------- */

static uint64_t iar_queue[16];          /* what ICC_IAR1_EL1 reads next; when they are used up, 1023 (none) */
static unsigned iar_count, iar_next;
static uint64_t mpidr = 0x100;          /* the core this runs on: core 1, as the phones number them */
static struct { const char *name; uint64_t value; } writes[64];
static unsigned nwrites;

static uint64_t
rsr64(const char *name)
{
	if (strcmp(name, "ICC_IAR1_EL1") == 0) {
		return iar_next < iar_count ? iar_queue[iar_next++] : 1023;
	}
	if (strcmp(name, "MPIDR_EL1") == 0) {
		return mpidr;
	}
	return 0;
}

static void
wsr64(const char *name, uint64_t value)
{
	if (nwrites < sizeof(writes) / sizeof(writes[0])) {
		writes[nwrites].name = name;
		writes[nwrites++].value = value;
	}
}

/* was that register written with that value (since the record was last cleared)? */
static bool
written(const char *name, uint64_t value)
{
	for (unsigned i = 0; i < nwrites; i++) {
		if (strcmp(writes[i].name, name) == 0 && writes[i].value == value) {
			return true;
		}
	}
	return false;
}

static bool
written_at_all(const char *name)
{
	for (unsigned i = 0; i < nwrites; i++) {
		if (strcmp(writes[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

#undef __builtin_arm_rsr64
#define __builtin_arm_rsr64(name)               rsr64(name)
#define __builtin_arm_wsr64(name, value)        wsr64((name), (value))
#define __builtin_arm_isb(what)                 ((void)(what))
#define ISB_SY                                  0xf

/* ---- what the kernel gives it ------------------------------------------------------------------ */

#define KERN_INVALID_ARGUMENT                   4
#define KERN_RESOURCE_SHORTAGE                  6
#define KERN_NAME_EXISTS                        13
#define LCK_GRP_NULL                            NULL
#define decl_simple_lock_data(class, name)      class pthread_mutex_t name
#define simple_lock_init(lock, flags)           pthread_mutex_init((lock), NULL)
#define simple_lock(lock, group)                pthread_mutex_lock(lock)
#define simple_unlock(lock)                     pthread_mutex_unlock(lock)
#define os_atomic_load(pointer, order)          __atomic_load_n((pointer), __ATOMIC_SEQ_CST)
#define os_atomic_store(pointer, value, order)  __atomic_store_n((pointer), (value), __ATOMIC_SEQ_CST)
#define os_atomic_dec(pointer, order)           __sync_sub_and_fetch((pointer), 1)

static int this_cpu = 1;
#define cpu_number()                            this_cpu
static struct { uint64_t cpu_decrementer; } cpu_data;
#define getCpuDatap()                           (&cpu_data)
static unsigned timer_ticks, signals, stops;
static void rtclock_intr(int user) { (void)user; timer_ticks++; }
static void cpu_signal_handler(void) { signals++; }
static void psci_smp_stop_here(void) { stops++; }
static unsigned ml_get_cpu_count(void) { return 2; }
static void ml_install_interrupt_handler(void *nub, int source, void *target, void (*handler)(void *, void *, void *, int),
    void *refcon) { (void)nub; (void)source; (void)target; (void)handler; (void)refcon; }

static void
kprintf(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	loader_log_line(line);
}

/* a panic ends the scenario's call to the code there and then: the test looks at what it said */
static jmp_buf on_panic;
static char panic_text[256];

static __attribute__((noreturn)) void
panic(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(panic_text, sizeof(panic_text), format, arguments);
	va_end(arguments);
	longjmp(on_panic, 1);
}

/* ---- the controller: its two regions in memory and the device tree's node ---------------------- */

/* (the architecture's numbers, the test's own: the same with the code of before, which has no names for them) */
#define TYPER_ESPI      (1u << 8)       /* GICD_TYPER.ESPI: the distributor has extended SPIs */
#define ICENABLERE0     0x1400          /* GICD_ICENABLER<n>E: the first 32 extended SPIs' */

#define GICD_PHYS       0x1000000ull
#define GICR_PHYS       0x2000000ull
#define FRAMES          2               /* a redistributor (two 64 KB frames) a core */

static uint64_t gicd_mem[0x10000 / 8];
static uint64_t gicr_mem[FRAMES * 0x20000 / 8];
static uint64_t dt_reg[4] = { GICD_PHYS, sizeof(gicd_mem), GICR_PHYS, sizeof(gicr_mem) };
static uint64_t dt_stride;
static bool dt_has_stride;

static vm_offset_t
ml_io_map(uint64_t phys, uint64_t size)
{
	if (phys == GICD_PHYS && size <= sizeof(gicd_mem)) {
		return (vm_offset_t)gicd_mem;
	}
	if (phys == GICR_PHYS && size <= sizeof(gicr_mem)) {
		return (vm_offset_t)gicr_mem;
	}
	return 0;
}

static uint64_t pe_arm_get_soc_base_phys(void) { return 0; }

static int
dt_lookup(const DTEntry search, const char *path, DTEntry *entry)
{
	(void)search;
	*entry = (DTEntry)dt_reg;
	return strcmp(path, "/arm-io/gic") == 0 ? kSuccess : kError;
}

static int
dt_property(const DTEntry entry, const char *name, void const **value, unsigned int *size)
{
	(void)entry;
	if (strcmp(name, "reg") == 0) {
		*value = dt_reg;
		*size = sizeof(dt_reg);
		return kSuccess;
	}
	if (strcmp(name, "redistributor-stride") == 0 && dt_has_stride) {
		*value = &dt_stride;
		*size = sizeof(dt_stride);
		return kSuccess;
	}
	return kError;
}
#define SecureDTLookupEntry     dt_lookup
#define SecureDTGetProperty     dt_property

#define GENERIC_GIC_V3_IRQ 1
#include "../arch/arm64/kernel/gic_v3_irq.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint32_t
reg32(uint64_t *region, uint32_t offset)
{
	uint32_t value;

	memcpy(&value, (uint8_t *)region + offset, sizeof(value));
	return value;
}

static void
set32(uint64_t *region, uint32_t offset, uint32_t value)
{
	memcpy((uint8_t *)region + offset, &value, sizeof(value));
}

/* each redistributor says whose it is: frame n is core n's (MPIDR n << 8), the last one marked */
static void
controller(uint32_t gicd_typer)
{
	for (unsigned n = 0; n < FRAMES; n++) {
		gicr_mem[(n * 0x20000 + GICR_TYPER) / 8] = (uint64_t)(n << 8) << 32 | (n == FRAMES - 1 ? GICR_TYPER_LAST : 0);
	}
	set32(gicd_mem, GICD_TYPER, gicd_typer);
}

/* the processor's bring-up; false if it panicked */
static bool
init(void)
{
	if (setjmp(on_panic) != 0) {
		return false;
	}
	gic_v3_irq_cpu_init();
	return true;
}

static void
take_interrupts(void)
{
	gic_irq_handler(NULL, NULL, NULL, 0);
}

/*
 * An extended SPI (4096) acknowledged: it is not taken for "no interrupt" (1020 to 1023 only), but masked in the
 * extended SPIs' own enable registers -- the distributor says it has them -- counted, and ended, and the loop goes on
 * to the next one. Left without its end, the timer and the signals between processors would never come again.
 */
static void
espi_ended(void)
{
	controller(0x1f | TYPER_ESPI);     /* 1024 SPIs' worth of lines; extended SPIs 4096 to 4127 */
	CHECK(init(), "bring-up panicked: %s", panic_text);
	nwrites = 0;
	iar_queue[iar_count++] = 4096;
	iar_queue[iar_count++] = GIC_INTID_VTIMER;
	take_interrupts();
	CHECK(written("ICC_EOIR1_EL1", 4096), "the extended SPI was ended");
	CHECK(written("ICC_EOIR1_EL1", GIC_INTID_VTIMER) && timer_ticks == 1, "the timer behind it was taken and ended");
	CHECK(reg32(gicd_mem, ICENABLERE0) == 1, "masked in GICD_ICENABLER0E: 0x%x", reg32(gicd_mem, ICENABLERE0));
	CHECK(gic_unhandled_count == 1 && gic_last_unhandled == 4096, "counted: %llu, last %u",
	    (unsigned long long)gic_unhandled_count, gic_last_unhandled);
	CHECK(gic_active_iar[this_cpu] == 0, "none left marked active");
}

/* the same without extended SPIs (an LPI, 8192, or a distributor without them): counted and ended, no register written */
static void
lpi_ended(void)
{
	controller(0x1f);
	CHECK(init(), "bring-up panicked: %s", panic_text);
	nwrites = 0;
	iar_queue[iar_count++] = 8192;
	iar_queue[iar_count++] = 4100;
	take_interrupts();
	CHECK(written("ICC_EOIR1_EL1", 8192) && written("ICC_EOIR1_EL1", 4100), "both ended");
	CHECK(reg32(gicd_mem, ICENABLERE0) == 0, "no extended SPI register written on a distributor without them");
	CHECK(gic_unhandled_count == 2, "counted: %llu", (unsigned long long)gic_unhandled_count);
}

/* 1020 to 1023: nothing was acknowledged, so nothing is ended */
static void
special_not_ended(void)
{
	controller(0x1f);
	CHECK(init(), "bring-up panicked: %s", panic_text);
	for (uint64_t intid = 1020; intid <= 1023; intid++) {
		nwrites = 0;
		iar_count = iar_next = 0;
		iar_queue[iar_count++] = intid;
		iar_queue[iar_count++] = GIC_INTID_VTIMER;
		take_interrupts();
		CHECK(!written_at_all("ICC_EOIR1_EL1"), "%llu: no end written", (unsigned long long)intid);
		CHECK(iar_next == 1, "%llu: the loop stopped there", (unsigned long long)intid);
	}
	CHECK(gic_unhandled_count == 0, "nothing counted");
}

/*
 * A core switched off (gic_v3_irq_cpu_shutdown(): everything in its redistributor off, Group 1 off in its CPU interface)
 * and started again by the firmware: its bring-up is done again, not skipped because its redistributor is known.
 */
static void
restart_after_shutdown(void)
{
	controller(0x1f);
	CHECK(init(), "bring-up panicked: %s", panic_text);
	CHECK(written("ICC_IGRPEN1_EL1", 1), "Group 1 on the first time");
	nwrites = 0;
	gic_v3_irq_cpu_shutdown();
	CHECK(written("ICC_IGRPEN1_EL1", 0), "off at the shutdown");
	set32(gicr_mem, 0x20000 + GICR_ISENABLER0, 0);  /* (what the shutdown's ICENABLER0 did to it) */
	nwrites = 0;
	CHECK(init(), "the second bring-up panicked: %s", panic_text);
	CHECK(written("ICC_IGRPEN1_EL1", 1), "Group 1 on again");
	CHECK(written("ICC_PMR_EL1", 0xff), "every priority accepted again");
	CHECK(reg32(gicr_mem, 0x20000 + GICR_ISENABLER0) & (1u << GIC_INTID_VTIMER), "the timer enabled again");
	CHECK(gicr_cpu_base[this_cpu] == (vm_offset_t)gicr_mem + 0x20000, "the same redistributor");
}

/* a redistributor-stride that is not a whole number of 64 KB frames is not used (the frames' own sizes are) and said */
static void
bad_stride_refused(void)
{
	controller(0x1f);
	dt_has_stride = true;
	dt_stride = 0x2;
	CHECK(init(), "bring-up panicked: %s", panic_text);
	CHECK(logged("redistributor-stride 0x2"), "said");
	CHECK(gicr_cpu_base[this_cpu] == (vm_offset_t)gicr_mem + 0x20000, "core 1's redistributor found");
}

/* a stride that is whole frames but goes past the region's end: the walk stops there, not after wrapping around */
static void
stride_past_the_end(void)
{
	controller(0x1f);
	dt_has_stride = true;
	dt_stride = 0xffffffffffff0000ull;
	CHECK(!init(), "core 1's redistributor not found");
	CHECK(strstr(panic_text, "no redistributor") != NULL, "%s", panic_text);
}

/* a distributor smaller than its registers' 64 KB is refused, before anything is mapped */
static void
distributor_too_small(void)
{
	controller(0x1f);
	dt_reg[1] = 0x1000;
	CHECK(!init(), "refused");
	CHECK(gicd_base == 0, "nothing mapped");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "espi_ended", espi_ended }, { "lpi_ended", lpi_ended }, { "special_not_ended", special_not_ended },
		{ "restart_after_shutdown", restart_after_shutdown }, { "bad_stride_refused", bad_stride_refused },
		{ "stride_past_the_end", stride_past_the_end }, { "distributor_too_small", distributor_too_small },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
