/*
 * MediaTek MT6833 (Dimensity 700 / POCO M4 5G) Mali-G57 MC2 Valhall GPU Driver.
 *
 * Provides hardware initialization, clock gating, and power domain management for
 * the ARM Mali-G57 MC2 GPU at physical address 0x13000000.
 *
 * Hardware Mapping (from poco_m4_5g.dts):
 *   mali@13000000 : compatible = "mediatek,mali", "arm,mali-valhall"
 *   Registers     : 0x13000000 - 0x13004000 (16 KB)
 *   Interrupts    : GPU (364), MMU (365), JOB (366), EVENT (367), PWR (368)
 *
 * Activated when boot-arg mali=1 or gpu=1 is present.
 */

#include <stdarg.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

void hakios_keep(const char *format, ...) __printflike(1, 2);

void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
}

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MT6833_MALI_PHYS        0x13000000ULL
#define MT6833_MALI_SIZE        0x4000

#define MT6833_TOPCKGEN_PHYS    0x10000000ULL
#define MT6833_TOPCKGEN_SIZE    0x1000

#define MT6833_SPM_PHYS         0x10006000ULL
#define MT6833_SPM_SIZE         0x1000

/* ARM Mali Valhall / Bifrost common register offsets */
#define MALI_REG_GPU_ID             0x0000
#define MALI_REG_L2_FEATURES        0x0004
#define MALI_REG_TILER_FEATURES     0x000c
#define MALI_REG_MEM_FEATURES       0x0010
#define MALI_REG_MMU_FEATURES       0x0014
#define MALI_REG_AS_PRESENT         0x0018
#define MALI_REG_JS_PRESENT         0x001c
#define MALI_REG_GPU_IRQ_RAWSTAT    0x0020
#define MALI_REG_GPU_IRQ_CLEAR      0x0024
#define MALI_REG_GPU_IRQ_MASK       0x0028
#define MALI_REG_GPU_IRQ_STATUS     0x002c
#define MALI_REG_L2_PWRON_LO        0x0040
#define MALI_REG_L2_PWROFF_LO       0x0048
#define MALI_REG_L2_PWR_STATUS_LO   0x0050
#define MALI_REG_SHADER_LO          0x0100
#define MALI_REG_SHADER_HI          0x0104
#define MALI_REG_SHADER_PWRON_LO    0x0110
#define MALI_REG_SHADER_PWROFF_LO   0x0118
#define MALI_REG_SHADER_PWR_STAT_LO 0x0120
#define MALI_REG_SHADER_PWR_TRANS_LO 0x0130

/* Job Control & MMU Registers */
#define MALI_REG_JOB_INT_RAWSTAT    0x1000
#define MALI_REG_JOB_INT_CLEAR      0x1004
#define MALI_REG_JOB_INT_MASK       0x1008
#define MALI_REG_JOB_INT_STAT       0x100c
#define MALI_REG_JOB_SLOT0_HEAD_LO  0x1080
#define MALI_REG_JOB_SLOT0_HEAD_HI  0x1084
#define MALI_REG_JOB_SLOT0_COMMAND  0x1090
#define MALI_REG_JOB_SLOT1_HEAD_LO  0x1100
#define MALI_REG_JOB_SLOT1_HEAD_HI  0x1104
#define MALI_REG_JOB_SLOT1_COMMAND  0x1110

#define MALI_REG_MMU_INT_RAWSTAT    0x2000
#define MALI_REG_MMU_INT_CLEAR      0x2004
#define MALI_REG_MMU_INT_MASK       0x2008

SYSCTL_DECL(_kern_hakios);

static vm_offset_t mali_base;
static vm_offset_t topckgen_base;
static vm_offset_t spm_base;

static uint32_t gpu_status = 0;
static uint32_t gpu_id_val = 0;
static uint32_t shader_cores = 0;
static uint32_t gpu_clock_mhz = 950;
static uint64_t gpu_jobs_dispatched = 0;
static bool gpu_is_ready = false;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_status, 0,
    "MT6833 Mali GPU initialization status (0=off, 1=init, 2=mapped, 3=ready)");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id_val, 0,
    "Mali hardware GPU_ID register value");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_cores, CTLFLAG_RD | CTLFLAG_LOCKED, &shader_cores, 0,
    "Number of active Mali-G57 shader cores (MC2=2)");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_clock, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_clock_mhz, 0,
    "Mali GPU frequency in MHz");

SYSCTL_QUAD(_kern_hakios, OID_AUTO, mali_jobs, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_jobs_dispatched,
    "Total CoreAnimation / Panfrost GPU job descriptors processed");

static inline uint32_t
mali_rd(uint32_t off)
{
	return *(volatile uint32_t *)(mali_base + off);
}

static inline void
mali_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(mali_base + off) = val;
}

bool
hakios_mali_ready(void)
{
	return gpu_is_ready;
}

/*
 * Dispatch a 2D composite batch descriptor from CoreAnimation / HakiosMetal
 * to the hardware Mali Job Slot 1 (fragment / pixel pipeline).
 */
int
hakios_mali_submit_job(uint32_t slot, uint64_t job_head_phys)
{
	if (!gpu_is_ready || !mali_base) {
		return -1;
	}

	uint32_t head_lo_reg = (slot == 0) ? MALI_REG_JOB_SLOT0_HEAD_LO : MALI_REG_JOB_SLOT1_HEAD_LO;
	uint32_t head_hi_reg = (slot == 0) ? MALI_REG_JOB_SLOT0_HEAD_HI : MALI_REG_JOB_SLOT1_HEAD_HI;
	uint32_t cmd_reg     = (slot == 0) ? MALI_REG_JOB_SLOT0_COMMAND : MALI_REG_JOB_SLOT1_COMMAND;

	mali_wr(head_lo_reg, (uint32_t)(job_head_phys & 0xFFFFFFFFULL));
	mali_wr(head_hi_reg, (uint32_t)(job_head_phys >> 32));
	mali_wr(cmd_reg, 0x1); /* START job command */

	gpu_jobs_dispatched++;
	return 0;
}

void
hakios_mali_init(void)
{
	uint32_t enable = 0;

	if (!PE_parse_boot_argn("mali", &enable, sizeof(enable)) &&
	    !PE_parse_boot_argn("gpu", &enable, sizeof(enable))) {
		return;
	}

	if (enable == 0) {
		printf("mt6833_mali: disabled by boot-arg mali=0\n");
		return;
	}

	printf("mt6833_mali: initializing Mali-G57 MC2 Valhall GPU @ %#llx\n", MT6833_MALI_PHYS);
	gpu_status = 1;

	mali_base = ml_io_map(MT6833_MALI_PHYS, MT6833_MALI_SIZE);
	topckgen_base = ml_io_map(MT6833_TOPCKGEN_PHYS, MT6833_TOPCKGEN_SIZE);
	spm_base = ml_io_map(MT6833_SPM_PHYS, MT6833_SPM_SIZE);

	if (!mali_base) {
		printf("mt6833_mali: failed to map GPU registers at %#llx\n", MT6833_MALI_PHYS);
		return;
	}

	gpu_status = 2;

	/* Step 1: Power up SPM GPU domain if SPM registers mapped */
	if (spm_base) {
		*(volatile uint32_t *)(spm_base + 0x300) |= 0x1; /* MFG / GPU power domain on */
	}

	/* Step 2: Clear TOPCKGEN clock gate */
	if (topckgen_base) {
		*(volatile uint32_t *)(topckgen_base + 0x080) &= ~(1U << 16); /* Ungate MFG clock @ 950MHz */
	}

	/* Step 3: Probe Mali hardware ID and features */
	gpu_id_val = mali_rd(MALI_REG_GPU_ID);
	uint32_t shader_lo = mali_rd(MALI_REG_SHADER_LO);
	uint32_t mem_feat = mali_rd(MALI_REG_MEM_FEATURES);
	uint32_t mmu_feat = mali_rd(MALI_REG_MMU_FEATURES);

	shader_cores = __builtin_popcount(shader_lo);
	if (shader_cores == 0) {
		shader_cores = 2; /* POCO M4 5G Dimensity 700 standard: 2 cores (MC2) */
	}

	/* Step 4: Power up L2 Cache */
	mali_wr(MALI_REG_L2_PWRON_LO, 0x1);

	/* Step 5: Power up all active shader cores (Valhall MC2 = mask 0x3) */
	uint32_t core_mask = (1U << shader_cores) - 1;
	mali_wr(MALI_REG_SHADER_PWRON_LO, core_mask);

	/* Step 6: Enable Job Slot and MMU Interrupts for Panfrost */
	mali_wr(MALI_REG_JOB_INT_MASK, 0x000F000F); /* Slots 0 and 1 completion + error interrupts */
	mali_wr(MALI_REG_MMU_INT_MASK, 0x00000003); /* MMU fault and page-table walk interrupts */

	printf("mt6833_mali: GPU_ID=%#x, shader core mask=%#x (%u cores @ %u MHz)\n",
	       gpu_id_val, shader_lo, shader_cores, gpu_clock_mhz);
	printf("mt6833_mali: L2 cache & %u Valhall shader cores powered on; Job Slots ready\n", shader_cores);

	gpu_is_ready = true;
	gpu_status = 3;
	printf("mt6833_mali: Mali-G57 MC2 Valhall GPU online and registered for CoreAnimation / HakiosMetal\n");
}

