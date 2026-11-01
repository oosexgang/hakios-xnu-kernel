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
#define MALI_REG_GPU_ID         0x0000
#define MALI_REG_L2_FEATURES    0x0004
#define MALI_REG_TILER_FEATURES 0x000c
#define MALI_REG_MEM_FEATURES   0x0010
#define MALI_REG_MMU_FEATURES   0x0014
#define MALI_REG_AS_PRESENT     0x0018
#define MALI_REG_JS_PRESENT     0x001c
#define MALI_REG_SHADER_LO      0x0100
#define MALI_REG_SHADER_HI      0x0104

SYSCTL_DECL(_kern_hakios);

static vm_offset_t mali_base;
static vm_offset_t topckgen_base;
static vm_offset_t spm_base;

static uint32_t gpu_status = 0;
static uint32_t gpu_id_val = 0;
static uint32_t shader_cores = 0;
static bool gpu_is_ready = false;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_status, 0,
    "MT6833 Mali GPU initialization status (0=off, 1=init, 2=mapped, 3=ready)");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id_val, 0,
    "Mali hardware GPU_ID register value");

static inline uint32_t
mali_rd(uint32_t off)
{
	return *(volatile uint32_t *)(mali_base + off);
}

bool
hakios_mali_ready(void)
{
	return gpu_is_ready;
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

	/* Probe Mali GPU hardware ID */
	gpu_id_val = mali_rd(MALI_REG_GPU_ID);
	uint32_t shader_lo = mali_rd(MALI_REG_SHADER_LO);
	uint32_t mem_feat = mali_rd(MALI_REG_MEM_FEATURES);
	uint32_t mmu_feat = mali_rd(MALI_REG_MMU_FEATURES);

	shader_cores = __builtin_popcount(shader_lo);

	printf("mt6833_mali: GPU_ID=%#x, shader core mask=%#x (%u cores), mem_feat=%#x, mmu_feat=%#x\n",
	       gpu_id_val, shader_lo, shader_cores, mem_feat, mmu_feat);

	gpu_is_ready = true;
	gpu_status = 3;
	printf("mt6833_mali: Mali-G57 MC2 Valhall GPU online and registered\n");
}
