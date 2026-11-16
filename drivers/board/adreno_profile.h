/*
 * What differs between the Adrenos this driver starts, as the driver wants it. The values are the
 * board's (gpu { } of arch/arm64/boot/dts/NAME/board.dts, through board/board.h); they come from the phone's device
 * tree (the SoC's -gpu.dtsi), Linux's adreno-gpulist.h and adreno_a6xx_gmu.c. A board has a profile
 * only if it lists the feature "gpu-a660" and has a gpu { } section.
 */
#ifndef HAKIOS_ADRENO_PROFILE_H
#define HAKIOS_ADRENO_PROFILE_H

#include "board.h"

struct adreno_speed { uint32_t khz, level; };
struct adreno_board_profile {
	const char *board, *model, *firmware_directory;
	const char *alt_model;                  /* what the speed bins alt_bins are sold as (NULL: none) */
	uint32_t alt_bins[2];
	uint32_t chip_id, gmem_bytes, primitive_fifo, hang_cycles;
	uint32_t uche_cmdq_config;              /* 0: not written */
	uint32_t speed_fuse, gaming_fuse, highest_bank_bit;
	uint32_t gcc_debug_gpucc;
	bool zap_must_authenticate;             /* stop when TrustZone does not confirm the zap shader */
	uint32_t speed_count, default_speed;    /* speeds[0] is "off": the board's speeds follow from 1 */
	struct adreno_speed speeds[6];
};

/* A642's patch high nibble encodes target, not patch: GMU expects it in [31:28]. */
static uint32_t
adreno642_gmu_chip_id(uint32_t chip)
{
	return (chip & 0xffff0000) | ((chip >> 8 & 0xff) << 12) |
	       ((chip & 0xf) << 8) | ((chip >> 4 & 0xf) << 28);
}

static struct adreno_board_profile adreno_of_board;

/* The profile of the board this kernel runs on; NULL when it has no GPU this driver knows. */
static const struct adreno_board_profile *
adreno_profile_for_board(void)
{
	const struct hakios_board *board = hakios_board();
	struct adreno_board_profile *p = &adreno_of_board;

	if (!hakios_board_has(HAKIOS_HAS_GPU_A660) || board->gpu.chip_id == 0 || board->gpu.speed_count == 0 ||
	    board->gpu.speed_count > 5) {
		return NULL;
	}
	p->board = board->name;
	p->model = board->gpu.model;
	p->firmware_directory = board->gpu.firmware;
	p->alt_model = board->gpu.alt_model;
	p->alt_bins[0] = board->gpu.alt_bins[0];
	p->alt_bins[1] = board->gpu.alt_bins[1];
	p->chip_id = board->gpu.chip_id;
	p->gmem_bytes = board->gpu.gmem_bytes;
	p->primitive_fifo = board->gpu.primitive_fifo;
	p->hang_cycles = board->gpu.hang_cycles;
	p->uche_cmdq_config = board->gpu.uche_cmdq_config;
	p->speed_fuse = board->gpu.speed_fuse;
	p->gaming_fuse = board->gpu.gaming_fuse;
	p->highest_bank_bit = board->gpu.highest_bank_bit;
	p->gcc_debug_gpucc = board->gpu.gcc_debug_gpucc;
	p->zap_must_authenticate = board->gpu.zap_must_authenticate != 0;
	p->speed_count = board->gpu.speed_count + 1u;
	p->default_speed = board->gpu.default_speed;
	p->speeds[0] = (struct adreno_speed){ 0, 0 };
	for (unsigned i = 0; i < board->gpu.speed_count; i++) {
		p->speeds[i + 1] = (struct adreno_speed){ board->gpu.speeds_khz[i], board->gpu.speed_levels[i] };
	}
	return p;
}
#endif
