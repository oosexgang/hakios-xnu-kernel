/* Part of the loader (included by loader.c, not compiled alone): the kernel on another core: the stub it starts in, its log read while it runs. */
/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args, probe_keys_pa, probe_slot_pa;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
/* hakios core-200 PAC probe: what the kernel's core reads for the IA key straight from physical
 * pac_std_keys (MMU/caches off), and the result of autia'ing the actual signed startup slot with it */
volatile uint64_t other_cpu_probe_key0 __attribute__((aligned(64)));
volatile uint64_t other_cpu_probe_key1 __attribute__((aligned(64)));
volatile uint64_t other_cpu_probe_autia __attribute__((aligned(64)));
uint64_t g_probe_keys_pa;   /* hakios: physical pac_std_keys, set while signing, read by the probe */
void other_cpu_entry(void);
void other_cpu_return_stub(void);
void secondary_cpu_entry(void);
/* hakios: what the first secondary core saw in secondary_cpu_entry (pacib fingerprints, IB key and
 * CPACR read-backs around a first FP instruction); written with the MMU off, read by the loader */
volatile uint64_t secondary_probe[8] __attribute__((aligned(64)));

/*
 * Where the kernel has the firmware start its other processors (/chosen/hakios,secondary-entry):
 * under Qualcomm's hypervisor the kernel's own writes to the pointer-authentication key registers
 * do not take, and a core started straight into the kernel would run with whatever keys it had
 * (and fault at the kernel's first signed pointer). So, as for the kernel's first core
 * (other_cpu_entry), the keys are put in place here, with the MMU and caches still off, before
 * going on to the kernel's reset vector. x0 (the CPU_ON context) = that vector's physical address.
 */
__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl secondary_cpu_entry\n"
        "secondary_cpu_entry:\n"
        /* exactly what other_cpu_entry does before the keys (on that core they then stay put):
         * vectors, CPACR, and the fixed bits of SCTLR with the MMU and caches off */
        "  msr daifset, #0xf\n"
        /* no instruction or translation of the run before: the core may have kept them through its CPU_OFF */
        "  ic iallu\n"
        "  tlbi vmalle1\n"
        "  dsb nsh\n"
        "  isb\n"
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* (constants and other_cpu_start only: the core reads memory with its caches off, and those
         * are what was written back before the kernel started; loader_ctx and g_probe_keys_pa may not be) */
        "  movz x9, #0x30, lsl #16\n"          /* CPACR: FP and SIMD allowed (0x300000) */
        "  msr cpacr_el1, x9\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  .arch_extension pauth\n"
        "  movz x15, #0x3514\n"
        "  movk x15, #0x809a, lsl #16\n"
        "  movk x15, #0x0004, lsl #32\n"       /* the TCR the kernel's first core was given */
        "  msr tcr_el1, x15\n"
        "  adrp x11, other_cpu_start\n"
        "  add  x11, x11, :lo12:other_cpu_start\n"
        "  ldr x11, [x11, #16]\n"               /* probe_keys_pa: physical pac_std_keys */
        "  cbz x11, 3f\n"                       /* no keys (as in other_cpu_entry): no keys, no probe */
        "  ldp x12, x13, [x11, #0]\n   msr APIAKeyLo_EL1, x12\n  msr APIAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #16]\n  msr APIBKeyLo_EL1, x12\n  msr APIBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #32]\n  msr APDAKeyLo_EL1, x12\n  msr APDAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #48]\n  msr APDBKeyLo_EL1, x12\n  msr APDBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #64]\n  msr APGAKeyLo_EL1, x12\n  msr APGAKeyHi_EL1, x13\n"
        "  isb\n"
        /* probe: fingerprint, IB key and CPACR; then a first FP instruction; again; then the keys once
         * more and a last fingerprint (x14 = value, x15 = modifier, as the kernel's "first heartbeat") */
        "  adrp x16, secondary_probe\n"
        "  add  x16, x16, :lo12:secondary_probe\n"
        "  movz x14, #0x4000\n  movk x14, #0x0700, lsl #16\n  movk x14, #0xfff0, lsl #32\n  movk x14, #0xffff, lsl #48\n"
        "  movz x15, #0x1234\n"
        "  mov x17, x14\n  pacib x17, x15\n  str x17, [x16, #0]\n"
        "  mrs x17, APIBKeyLo_EL1\n  str x17, [x16, #8]\n"
        "  mrs x17, cpacr_el1\n  str x17, [x16, #16]\n"
        "  .inst 0x9e6703e0\n"                  /* fmov d0, xzr: a first FP instruction */
        "  mrs x17, cpacr_el1\n  str x17, [x16, #24]\n"
        "  mov x17, x14\n  pacib x17, x15\n  str x17, [x16, #32]\n"
        "  mrs x17, APIBKeyLo_EL1\n  str x17, [x16, #40]\n"
        "  ldp x12, x13, [x11, #0]\n   msr APIAKeyLo_EL1, x12\n  msr APIAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #16]\n  msr APIBKeyLo_EL1, x12\n  msr APIBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #32]\n  msr APDAKeyLo_EL1, x12\n  msr APDAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #48]\n  msr APDBKeyLo_EL1, x12\n  msr APDBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #64]\n  msr APGAKeyLo_EL1, x12\n  msr APGAKeyHi_EL1, x13\n"
        "  movz x9, #0x30, lsl #16\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  mov x17, x14\n  pacib x17, x15\n  str x17, [x16, #48]\n"
        "  movz x17, #0x5ec0\n  str x17, [x16, #56]\n"
        "  dsb sy\n"
        /* one call into the firmware (PSCI_VERSION) with the keys in place: the hypervisor takes the
         * core's registers as they are now, keys included, instead of what it held from CPU_ON */
        "3: mov x20, x0\n"
        "  adrp x10, psci_by_hvc\n"
        "  ldr x9, [x10, :lo12:psci_by_hvc]\n"
        "  movz x0, #0x0000\n"
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 1f\n"
        "  smc #0\n"
        "  b 2f\n"
        "1: hvc #0\n"
        "2: isb\n"
        "  mov x9, x20\n"
        "  mov x0, #0\n"
        "  br x9\n"
);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* no instruction or translation of the run before: the core may have kept them through its CPU_OFF */
        "  ic iallu\n"
        "  tlbi vmalle1\n"
        "  dsb nsh\n"
        "  isb\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        /*
         * The pointer-authentication keys and TCR, before the kernel: under Qualcomm's hypervisor the
         * kernel's own writes to the key registers do not take -- what a core holds is what was written
         * to it first. So the first write is the real key, every key (an earlier probe here wrote a zero
         * IB key first, and that core then authenticated with zero while every other core used the real
         * one). secondary_cpu_entry does the same for the kernel's other cores.
         */
        "  .arch_extension pauth\n"
        "  ldr x11, [x0, #16]\n"               /* probe_keys_pa: physical pac_std_keys */
        "  movz x15, #0x3514\n"
        "  movk x15, #0x809a, lsl #16\n"
        "  movk x15, #0x0004, lsl #32\n"       /* x15 = 0x00000004809a3514 = loader's final TCR */
        "  msr tcr_el1, x15\n"
        /* no keys (a kernel without __pac_keys, a core without pointer authentication, whose key
         * registers do not exist): nothing to put in place */
        "  cbz x11, 1f\n"
        "  ldp x12, x13, [x11, #0]\n   msr APIAKeyLo_EL1, x12\n  msr APIAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #16]\n  msr APIBKeyLo_EL1, x12\n  msr APIBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #32]\n  msr APDAKeyLo_EL1, x12\n  msr APDAKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #48]\n  msr APDBKeyLo_EL1, x12\n  msr APDBKeyHi_EL1, x13\n"
        "  ldp x12, x13, [x11, #64]\n  msr APGAKeyLo_EL1, x12\n  msr APGAKeyHi_EL1, x13\n"
        "  isb\n"
        "1: dsb sy\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  dsb sy\n"                            /* (the log's place and the reason seen before "done": the MMU is off) */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
	uint64_t crumb;                 /* the last loader_checkpoint(n) the kernel passed */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */
#define OVERDUE_SECONDS         180     /* past its time, for its way out: then a kernel that still beats is taken for hung */

static void wdt_pet(void);
static int wdt_armed;
/*
 * Dead-man switch: the watchdog is only fed while the host has spoken within HOST_SILENCE_SECONDS.
 * A loader the host can no longer reach (its USB wedged, or the loader itself stuck) lets the
 * watchdog reset the phone, which then comes back up in Android on its own.
 */
#define HOST_SILENCE_SECONDS    180
static uint64_t host_last_cmd;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The kernel's log as the kernel describes it (its head, its ring) is the kernel's own memory, which a kernel gone wrong may
 * have written over: believed only within a size no log of its has (64 MB in all), so that the sums below do not wrap.
 */
#define KERNEL_LOG_MOST         (64ull << 20)

static int
kernel_log_believable(const struct kernel_live *live)
{
	return live->log_head <= KERNEL_LOG_MOST && live->log_ring <= KERNEL_LOG_MOST &&
	    live->log_head + live->log_ring <= KERNEL_LOG_MOST;
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	if (!kernel_log_believable(live)) {
		print("the kernel's log is described as %x + %x bytes: not believed, not handed over\n", live->log_head, live->log_ring);
		return;
	}
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	{
		static int secondary_probe_said;

		flush_dcache((uint64_t)(uintptr_t)secondary_probe, (uint64_t)(uintptr_t)secondary_probe + sizeof(secondary_probe));
		if (!secondary_probe_said && secondary_probe[7] == 0x5ec0) {
			secondary_probe_said = 1;
			print("secondary probe: pacib %x ib %x cpacr %x | after fp: cpacr %x pacib %x ib %x | rewritten: pacib %x\n",
			    secondary_probe[0], secondary_probe[1], secondary_probe[2], secondary_probe[3], secondary_probe[4],
			    secondary_probe[5], secondary_probe[6]);
		}
	}
	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n >= room) { \
	        take = 0; \
	} else if (take > room - n) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!kernel_log_believable(live)) {
		static const char odd[] = "(the kernel describes its log in sizes no log has: not read)\n";

		PUT(odd, sizeof(odd) - 1);
	} else if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}
