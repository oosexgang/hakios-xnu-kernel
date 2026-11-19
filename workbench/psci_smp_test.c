/*
 * The other processors of the generic boards (arch/arm64/kernel/psci_smp.c), compiled as it is, against a firmware
 * the test plays: a call to it (CPU_ON, CPU_OFF, AFFINITY_INFO) goes to firmware() below instead of SMC or HVC, and
 * what a started core does -- run, take an exception on its way in, end the system -- the test does in its place,
 * switching the number of the core "this" code runs on. The clock is the test's too: it moves only when it is read
 * or waited on, so a wait of seconds takes no time, and always ends.
 *
 *   psci_smp_test SCENARIO          one scenario a process: the code keeps its state in statics
 */
#include <setjmp.h>
#include <stdarg.h>

#include "fakekernel.h"

extern void loader_log_line(const char *line);

/* the kernel's own header, for the stages and the prototypes (arm64/psci_smp.h is the fake kernel's, empty) */
#define __dead2                                 __attribute__((noreturn))
#include "../arch/arm64/kernel/psci_smp.h"

/* ---- the clock: a read moves it on by 100 us, a wait by what was waited ------------------------- */

static uint64_t now_ns;

static uint64_t test_clock(void) { return now_ns += 100 * NSEC_PER_USEC; }
static void test_to_abs(uint64_t ns, uint64_t *abs) { *abs = ns; }
static void test_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline) { *deadline = now_ns + (uint64_t)interval * scale; }
static void test_delay(uint32_t interval, uint32_t scale);      /* (below: a core the test plays may move meanwhile) */
#define mach_absolute_time()                    test_clock()
#define nanoseconds_to_absolutetime(ns, abs)    test_to_abs((ns), (abs))
#define clock_interval_to_deadline(i, s, d)     test_deadline((i), (s), (d))
#define delay_for_interval(i, s)                test_delay((i), (s))

/* ---- the processors ---------------------------------------------------------------------------- */

#define CORES   3                               /* the kernel's cpus 0 (the boot processor), 1 and 2 */

typedef struct { uint32_t cpu_phys_id; } cpu_data_t;
static cpu_data_t cpu_data[CORES] = { { 0x000 }, { 0x100 }, { 0x200 } };     /* MPIDR: core n is n << 8 */
static int this_cpu;
#define cpu_number()                            this_cpu
static cpu_data_t *cpu_datap(int cpu) { return cpu >= 0 && cpu < CORES ? &cpu_data[cpu] : NULL; }
static int ml_get_max_cpu_number(void) { return CORES - 1; }
static struct { int cpu_id; } boot_processor;
#define master_processor                        (&boot_processor)

/* (what psci_smp.c's slower cores need to compile: none of them is used here) */
typedef struct { uint32_t cpu_id, phys_id, l2_access_penalty; } test_topology_cpu_t;
typedef struct { unsigned int num_cpus; test_topology_cpu_t cpus[MAX_CPUS]; } test_topology_info_t;
static test_topology_info_t test_topology;
#define ml_topology_info_t                      test_topology_info_t
#define ml_get_topology_info()                  (&test_topology)
typedef struct { int state; } *processor_t;
#define PROCESSOR_NULL                          ((processor_t)0)
#define PROCESSOR_IDLE                          4
static processor_t processor_array[MAX_CPUS];
static void thread_bind(processor_t processor) { (void)processor; }

/* the processor's registers and instructions: none of them does anything here */
#define __builtin_arm_wsr(name, value)          ((void)(name), (void)(value))
#define __builtin_arm_wsr64(name, value)        ((void)(name), (void)(value))
#define __builtin_arm_dsb(what)                 ((void)(what), __sync_synchronize())
#define __builtin_arm_sev()                     ((void)0)
#define __builtin_arm_wfi()                     ((void)0)
#define DSB_SY                                  0xf
#define DAIFSC_ALL                              0xf
#define CNTV_CTL_EL0_IMASKED                    2

#define os_atomic_load(pointer, order)          __atomic_load_n((pointer), __ATOMIC_SEQ_CST)
#define os_atomic_store(pointer, value, order)  __atomic_store_n((pointer), (value), __ATOMIC_SEQ_CST)
#define os_atomic_or(pointer, value, order)     __atomic_fetch_or((pointer), (value), __ATOMIC_SEQ_CST)
#define os_atomic_andnot(pointer, value, order) __atomic_fetch_and((pointer), ~(value), __ATOMIC_SEQ_CST)

/* the kernel's image: no segment to clean; its reset vector, at an address of its own */
static int _mh_execute_header;
static void *getsegdatafromheader(void *header, const char *name, unsigned long *size) { (void)header; (void)name; *size = 0; return NULL; }
static char LowResetVectorBase[16];
static uint64_t ml_static_vtop(vm_offset_t address) { return (uint64_t)address; }
unsigned long gVirtBase, gPhysBase;
static void CleanPoC_DcacheRegion(vm_offset_t address, size_t size) { (void)address; (void)size; }
static void test_flush(vm_offset_t address, size_t size);     /* (below: what a core wrote with its cache off appears there) */
#define FlushPoC_DcacheRegion(address, size)    test_flush((address), (size))

/* the interrupt controller: who was sent the "stop" signal */
static uint32_t stops_sent[16];
static unsigned nstops;
static void (*on_stop_signal)(uint32_t phys_id);
void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
	if (nstops < sizeof(stops_sent) / sizeof(stops_sent[0])) {
		stops_sent[nstops++] = phys_id;
	}
	if (on_stop_signal != NULL) {
		on_stop_signal(phys_id);
	}
}
void gic_v3_irq_cpu_shutdown(void) {}
static bool stop_sent_to(uint32_t phys_id)
{
	for (unsigned i = 0; i < nstops; i++) {
		if (stops_sent[i] == phys_id) {
			return true;
		}
	}
	return false;
}

static unsigned loader_returns;
void loader_return(void) { loader_returns++; }

/* the device tree's /chosen: psci-method "hvc" when the test says so */
static const char *psci_method;
static int
dt_lookup(const DTEntry search, const char *path, DTEntry *entry)
{
	(void)search;
	*entry = (DTEntry)&psci_method;
	return strcmp(path, "/chosen") == 0 ? kSuccess : kError;
}
static int
dt_property(const DTEntry entry, const char *name, void const **value, unsigned int *size)
{
	(void)entry;
	if (strcmp(name, "psci-method") == 0 && psci_method != NULL) {
		*value = psci_method;
		*size = (unsigned int)strlen(psci_method) + 1;
		return kSuccess;
	}
	return kError;
}
#define SecureDTLookupEntry     dt_lookup
#define SecureDTGetProperty     dt_property

/* what the code says, into the log the test reads (and no format checks of the host's: %lld of an int64_t is the kernel's) */
static void
test_printf(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	loader_log_line(line);
}
static int
test_snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int n;

	va_start(arguments, format);
	n = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return n;
}
#define printf          test_printf
#define snprintf        test_snprintf

/* ---- the firmware ------------------------------------------------------------------------------ */

static uint64_t firmware(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/*
 * psci_call()'s two instructions are the firmware's door. Here: its register variables become plain ones (__asm__ with
 * an argument is nothing), and "__asm__ volatile (...)" an empty asm statement and a call of firmware() with them.
 */
#define __asm__(...)
#define volatile(...)   ("" ::: "memory"); x0 = firmware(x0, x1, x2, x3)
#define GENERIC_PSCI_SMP 1
#include "../arch/arm64/kernel/psci_smp.c"
#undef volatile
#undef __asm__
#undef printf
#undef snprintf

#define PSCI_CPU_ON_SUCCESS     0
#define PSCI_ALREADY_ON         (-4)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int64_t cpu_on_answer = PSCI_CPU_ON_SUCCESS;
static uint64_t cpu_on_target, cpu_on_entry, cpu_on_context;
static unsigned cpu_on_calls, hvc_calls, smc_calls;
static void (*on_cpu_on)(uint64_t target);      /* what the started core does, before CPU_ON comes back */
static jmp_buf core_gone;                       /* where CPU_OFF takes a core the test plays */

static uint64_t
firmware(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	if (psci_smp_hvc) {
		hvc_calls++;
	} else {
		smc_calls++;
	}
	switch (function) {
	case PSCI_CPU_ON:
		cpu_on_calls++;
		cpu_on_target = arg1;
		cpu_on_entry = arg2;
		cpu_on_context = arg3;
		if (cpu_on_answer == PSCI_CPU_ON_SUCCESS && on_cpu_on != NULL) {
			on_cpu_on(arg1);
		}
		return (uint64_t)cpu_on_answer;
	case PSCI_CPU_OFF:
		longjmp(core_gone, 1);
	case PSCI_AFFINITY_INFO:
		for (int cpu = 0; cpu < CORES; cpu++) {
			if (cpu_data[cpu].cpu_phys_id == arg1) {
				return psci_smp_cpu_stage[cpu] == PSCI_SMP_CPU_OFF ? PSCI_AFFINITY_OFF : 0;
			}
		}
		return (uint64_t)-2;    /* INVALID_PARAMETERS */
	}
	return (uint64_t)-1;            /* NOT_SUPPORTED */
}

/* while the boot processor waits for a core: what the test has the others do meanwhile */
static void (*meanwhile)(unsigned flushes);
static unsigned flushes;

static void
test_flush(vm_offset_t address, size_t size)
{
	(void)address; (void)size;
	flushes++;
	if (meanwhile != NULL) {
		meanwhile(flushes);
	}
}

static void
test_delay(uint32_t interval, uint32_t scale)
{
	now_ns += (uint64_t)interval * scale;
}

/* ---- the scenarios ----------------------------------------------------------------------------- */

/*
 * The firmware refuses (ALREADY_ON: a core someone else runs): the start fails, and that core is not counted among
 * the started -- nobody waits for it to stop at the end, nor sends it anything. The call went by the conduit the
 * loader named (HVC), to the kernel's reset vector.
 */
static void
refused_start_not_counted(void)
{
	psci_method = "hvc";
	cpu_on_answer = PSCI_ALREADY_ON;
	CHECK(psci_smp_start_cpu(1) == KERN_FAILURE, "refused");
	CHECK(cpu_on_calls == 1 && cpu_on_target == 0x100, "CPU_ON for MPIDR 0x%llx", (unsigned long long)cpu_on_target);
	CHECK(cpu_on_entry == (uint64_t)(uintptr_t)LowResetVectorBase && cpu_on_context == 0, "entry 0x%llx",
	    (unsigned long long)cpu_on_entry);
	CHECK(hvc_calls == 1 && smc_calls == 0, "HVC %u, SMC %u", hvc_calls, smc_calls);
	CHECK(psci_smp_started == 0, "not counted: 0x%x", psci_smp_started);
	CHECK(psci_smp_cpu_stage[1] == 0, "stage %u", psci_smp_cpu_stage[1]);
	CHECK(logged("already on"), "said why");
}

/*
 * Core 1 is started, runs, and ends the system (a panic on its way in, which returns to the loader) before the boot
 * processor's CPU_ON has come back -- the thread that asked can be preempted right there. It must know it is not
 * alone: hand the return over to the boot processor and switch itself off. Counted only after CPU_ON came back, it
 * found no other core started, and went on to the loader itself, the boot processor still running.
 */
static bool core1_returned, core1_off;

static void
core1_ends_the_system(uint64_t target)
{
	CHECK(target == 0x100, "core 1 started");
	this_cpu = 1;
	if (setjmp(core_gone) == 0) {
		psci_smp_stop_others();
		core1_returned = true;
	} else {
		core1_off = true;
	}
	this_cpu = 0;
}

static void
new_core_ends_system_at_once(void)
{
	on_cpu_on = core1_ends_the_system;
	(void)psci_smp_start_cpu(1);
	CHECK(!core1_returned, "core 1 took itself for the only processor and went on to the boot loader");
	CHECK(core1_off, "core 1 switched itself off");
	CHECK(psci_smp_boot_wanted, "the boot processor was asked to make the return");
	CHECK(stop_sent_to(0x000), "the boot processor was signalled");
}

/*
 * Core 1's start is given up on (no sign of it in 5 seconds); while core 2 is waited for, core 1 takes an
 * exception on its way in after all and leaves its note. That note is core 1's (its MPIDR says so), not core 2's:
 * core 2, which comes, is started and counted, and is told to stop at the end. Taken for core 2's, core 2 was
 * reported failed and dropped from the started, and at the return to the loader it was left running.
 */
static void
late_fault_of_core1(unsigned n)
{
	if (n == 1) {
		psci_smp_start_fault.mpidr = 0x80000100;        /* MPIDR_EL1 of core 1 (bit 31 reads as one) */
		psci_smp_start_fault.esr = 0x96000004;
		psci_smp_start_fault.valid = 1;
	}
	if (n == 3) {
		psci_smp_cpu_stage[2] = PSCI_SMP_CPU_RUNNING;   /* core 2 joins the scheduler */
	}
}

static void
stops(uint32_t phys_id)
{
	if (phys_id == 0x200) {
		psci_smp_cpu_stage[2] = PSCI_SMP_CPU_OFF;
	}
}

static void
late_fault_of_another_core(void)
{
	CHECK(psci_smp_start_cpu(1) == KERN_FAILURE, "core 1 never comes");
	CHECK(logged("has not joined"), "said so");
	meanwhile = late_fault_of_core1;
	flushes = 0;
	CHECK(psci_smp_start_cpu(2) == KERN_SUCCESS, "core 2 comes");
	meanwhile = NULL;
	CHECK(psci_smp_started & (1u << 2), "core 2 counted: 0x%x", psci_smp_started);

	on_stop_signal = stops;
	psci_smp_stop_others();
	CHECK(stop_sent_to(0x200), "core 2 told to stop at the end");
	CHECK(!logged("psci: cpu 2 did not stop"), "core 2 stopped");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "refused_start_not_counted", refused_start_not_counted },
		{ "new_core_ends_system_at_once", new_core_ends_system_at_once },
		{ "late_fault_of_another_core", late_fault_of_another_core },
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
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
