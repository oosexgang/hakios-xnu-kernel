/*
 * The GPU of the MT6768, a Mali-G52 MC2 at 0x13040000, and what has to happen before it answers.
 *
 * A Bifrost Mali needs no firmware -- unlike the Adreno of the other phone, there is no
 * processor of its own to load and no signed image to have checked -- so waking it is power and
 * clocks and nothing else, in the order MediaTek's own kernel does it:
 *
 *   rails    VGPU and VSRAM_GPU from the PMIC (mtkpwrap.h): usually left on by the preloader
 *   clock    the "mfg_sel" mux in topckgen, which feeds the whole GPU block -- needed running
 *            before any of its power domains is switched on
 *   power    four domains in the power controller (SPM): MFG_ASYNC, the bridge to the rest of
 *            the chip; MFG, the GPU block; and MFG_CORE0 and _CORE1, its two shader cores. The
 *            block's SRAM is woken with each, and once MFG is up the bus protection that kept
 *            the rest of the chip from talking to a block with no power is released
 *   gate     one clock gate inside the block itself, "BG3D" in mfgcfg at 0x13000000
 *
 * after which the GPU's own registers can be read: which GPU it is, what it has, and whether its
 * L2, tiler and shader cores power up when asked.
 *
 * Sources: Linux drivers/clk/mediatek/clk-mt6768-pg.c (the power sequences, word for word),
 * clk-mt6768.c (the mux and the gate), drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.c
 * (the order), all from MediaTek's own 4.14 tree; drivers/gpu/drm/panfrost/panfrost_regs.h for the
 * GPU's registers.
 */
#ifndef MTKMALI_H
#define MTKMALI_H

#include <stdint.h>

/* ---- the power controller ---- */

#define SPM_BASE             0x10006000ULL
#define SPM_PWR_STATUS       0x180
#define SPM_PWR_STATUS_2ND   0x184

#define PWR_RST_B            (1u << 0)
#define PWR_ISO              (1u << 1)
#define PWR_ON               (1u << 2)
#define PWR_ON_2ND           (1u << 3)
#define PWR_CLK_DIS          (1u << 4)
#define PWR_SRAM_PDN         (1u << 8)
#define PWR_SRAM_PDN_ACK     (1u << 12)

struct mali_domain {
	const char *name;
	uint32_t con;           /* its control register in the SPM */
	uint32_t status;        /* its bit in PWR_STATUS and PWR_STATUS_2ND */
	int sram;               /* it has SRAM to wake and put to sleep */
};

/* In the order they come up; they go down in the other. */
static const struct mali_domain mali_domains[4] = {
	{ "MFG_ASYNC", 0x334, 1u << 14, 0 },
	{ "MFG",       0x338, 1u << 11, 1 },
	{ "MFG_CORE0", 0x34c, 1u << 12, 1 },
	{ "MFG_CORE1", 0x310, 1u << 13, 1 },
};

/* The bus protection between the GPU block and the rest of the chip, in infracfg. */
#define INFRA_TOPAXI_PROTECTEN_STA1  0x228
#define INFRA_TOPAXI_PROTECTEN_SET   0x2a0
#define INFRA_TOPAXI_PROTECTEN_CLR   0x2a4
#define MFG_PROT_STEP1       (1u << 25)
#define MFG_PROT_STEP2       ((1u << 21) | (1u << 22))

/* ---- the clocks ---- */

#define CLK_CFG_1            0x50       /* in topckgen */
#define CLK_CFG_1_SET        0x54
#define CLK_CFG_1_CLR        0x58
#define MFG_SEL_FIELD        (3u << 0)
#define MFG_SEL_PDN          (1u << 7)
#define MFG_SEL_SYSPLL_D3    2u         /* 364 MHz, off the main PLL that is always running */

#define MFGCFG_BASE          0x13000000ULL
#define MFG_CG_CON           0x000      /* a bit set is a clock held off */
#define MFG_CG_SET           0x004
#define MFG_CG_CLR           0x008
#define MFG_CG_BG3D          (1u << 0)

/* ---- the GPU's own registers ---- */

#define MALI_BASE            0x13040000ULL
#define GPU_ID               0x000
#define GPU_L2_FEATURES      0x004
#define GPU_CORE_FEATURES    0x008
#define GPU_TILER_FEATURES   0x00c
#define GPU_MEM_FEATURES     0x010
#define GPU_MMU_FEATURES     0x014
#define GPU_AS_PRESENT       0x018
#define GPU_JS_PRESENT       0x01c
#define GPU_INT_RAWSTAT      0x020
#define GPU_INT_CLEAR        0x024
#define GPU_INT_MASK         0x028
#define GPU_CMD              0x030
#define GPU_STATUS           0x034
#define GPU_THREAD_MAX       0x0a0
#define GPU_SHADER_PRESENT   0x100
#define GPU_TILER_PRESENT    0x110
#define GPU_L2_PRESENT       0x120
#define GPU_SHADER_READY     0x140
#define GPU_TILER_READY      0x150
#define GPU_L2_READY         0x160
#define GPU_SHADER_PWRON     0x180
#define GPU_TILER_PWRON      0x190
#define GPU_L2_PWRON         0x1a0
#define GPU_SHADER_PWROFF    0x1c0
#define GPU_TILER_PWROFF     0x1d0
#define GPU_L2_PWROFF        0x1e0

/* The job manager, and one of its slots. */
#define JOB_INT_RAWSTAT      0x1000
#define JOB_INT_CLEAR        0x1004
#define JOB_INT_MASK         0x1008
#define JS_BASE(n)           (0x1800 + (n) * 0x80)
#define JS_HEAD_LO           0x00
#define JS_STATUS            0x24
#define JS_HEAD_NEXT_LO      0x40
#define JS_HEAD_NEXT_HI      0x44
#define JS_AFFINITY_NEXT_LO  0x50
#define JS_AFFINITY_NEXT_HI  0x54
#define JS_CONFIG_NEXT       0x58
#define JS_COMMAND_NEXT      0x60
#define JS_COMMAND_START     0x01
/* address space 0, the caches cleaned and invalidated before and after, the usual priority */
#define JS_CONFIG_AS0_FLUSHED ((3u << 8) | (3u << 12) | (8u << 16))

/* Address space 0 of the GPU's MMU. */
#define AS0_BASE             0x2400
#define AS_TRANSTAB_LO       0x00
#define AS_MEMATTR_LO        0x08
#define AS_MEMATTR_HI        0x0c
#define AS_COMMAND           0x18
#define AS_FAULTSTATUS       0x1c
#define AS_FAULTADDRESS_LO   0x20
#define AS_STATUS            0x28
#define AS_TRANSCFG_LO       0x30
#define AS_TRANSCFG_HI       0x34
#define AS_COMMAND_UPDATE    0x01
#define AS_STATUS_ACTIVE     (1u << 0)
#define AS_TRANSCFG_IDENTITY 2u         /* a GPU address is the physical address: no tables */

/* The smallest job there is: write a value somewhere (type 2). */
#define MALI_JOB_WRITE_VALUE 2u
#define MALI_WRITE_IMM32     6u

#define GPU_CMD_SOFT_RESET   0x01
#define GPU_IRQ_RESET_COMPLETED (1u << 8)

/* ---- waiting, with a limit on it ---- */

/* Until (register & mask) == want, for at most `ms`. 1 if it got there. */
static int mali_wait(uint64_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;

	do {
		if ((rd32(reg) & mask) == want) {
			return 1;
		}
	} while (dbg_ticks() < end);
	return 0;
}

static void mali_set(uint64_t reg, uint32_t bits)
{
	wr32(reg, rd32(reg) | bits);
}

static void mali_clear(uint64_t reg, uint32_t bits)
{
	wr32(reg, rd32(reg) & ~bits);
}

/* ---- one domain up, or down ---- */

/*
 * Up: power, and its second switch; wait until the controller says both are on; then let the
 * clock in, drop the isolation and release the reset; and wake the SRAM, if it has any, waiting
 * until the SRAM says it is awake. 0 means it never said: the step that stalled is in *stage.
 */
static int mali_domain_on(const struct mali_domain *d, int *stage)
{
	uint64_t con = SPM_BASE + d->con;

	*stage = 1;
	mali_set(con, PWR_ON);
	mali_set(con, PWR_ON_2ND);
	if (!mali_wait(SPM_BASE + SPM_PWR_STATUS, d->status, d->status, 20) ||
	    !mali_wait(SPM_BASE + SPM_PWR_STATUS_2ND, d->status, d->status, 20)) {
		return 0;
	}
	*stage = 2;
	mali_clear(con, PWR_CLK_DIS);
	mali_clear(con, PWR_ISO);
	mali_set(con, PWR_RST_B);
	if (d->sram) {
		*stage = 3;
		mali_clear(con, PWR_SRAM_PDN);
		if (!mali_wait(con, PWR_SRAM_PDN_ACK, 0, 20)) {
			return 0;
		}
	}
	*stage = 0;
	return 1;
}

/*
 * Down, the way it came up but backwards: SRAM to sleep, isolate, clock off, reset, power off. It
 * goes all the way even when a step does not answer -- a domain that only half came up has to be
 * switched off too -- and says afterwards whether every step did.
 */
static int mali_domain_off(const struct mali_domain *d)
{
	uint64_t con = SPM_BASE + d->con;
	int answered = 1;

	if (d->sram) {
		mali_set(con, PWR_SRAM_PDN);
		answered = mali_wait(con, PWR_SRAM_PDN_ACK, PWR_SRAM_PDN_ACK, 20);
	}
	mali_set(con, PWR_ISO);
	mali_set(con, PWR_CLK_DIS);
	mali_clear(con, PWR_RST_B);
	mali_clear(con, PWR_ON);
	mali_clear(con, PWR_ON_2ND);
	return mali_wait(SPM_BASE + SPM_PWR_STATUS, d->status, 0, 20) &&
	    mali_wait(SPM_BASE + SPM_PWR_STATUS_2ND, d->status, 0, 20) && answered;
}

/* The GPU block's side of the bus: protection off once MFG has power, on again before it goes. */
static void mali_bus_open(void)
{
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_CLR, MFG_PROT_STEP2);
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_CLR, MFG_PROT_STEP1);
	__asm__ volatile("dsb sy");
}

static int mali_bus_close(void)
{
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_SET, MFG_PROT_STEP1);
	if (!mali_wait(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_STA1, MFG_PROT_STEP1, MFG_PROT_STEP1, 20)) {
		return 0;
	}
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_SET, MFG_PROT_STEP2);
	return mali_wait(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_STA1, MFG_PROT_STEP2, MFG_PROT_STEP2, 20);
}

/* What the product number of GPU_ID says, for the ones this phone could plausibly have. */
static const char *mali_name(uint32_t id)
{
	switch (id >> 16) {
	case 0x7212: return "Mali-G52";
	case 0x7402: return "Mali-G52 r1";
	case 0x7211: return "Mali-G76";
	case 0x7093: return "Mali-G31";
	case 0x9093: return "Mali-G57";
	default:     return "an unfamiliar Mali";
	}
}

#endif /* MTKMALI_H */
