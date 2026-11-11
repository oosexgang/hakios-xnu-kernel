/* Board data from OKX 08bbb0c150ea: shima-gpu.dtsi, shima.dtsi,
 * adreno-gpulist.h and adreno_a6xx_gmu.c. Existing redwood settings preserved.
 * Both phones use LPDDR4X: a6xx_init overrides highest_bank_bit to 14.
 */
#ifndef REDWOOD_ADRENO_PROFILE_H
#define REDWOOD_ADRENO_PROFILE_H
struct adreno_speed { uint32_t khz, level; };
struct adreno_board_profile {
	const char *board, *model, *firmware_directory;
	uint32_t chip_id, gmem_bytes, primitive_fifo, hang_cycles;
	uint32_t speed_fuse, gaming_fuse, highest_bank_bit;
	uint32_t gcc_debug_gpucc;
	uint32_t speed_count, default_speed;
	struct adreno_speed speeds[6];
};
static const struct adreno_board_profile adreno_redwood = {
	.board = "redwood", .model = "Adreno 642L", .firmware_directory = "fw",
	.chip_id = 0x06030500, .gmem_bytes = 512 * 1024,
	.primitive_fifo = 0x00200000, .hang_cycles = 0x1fffff,
	.speed_fuse = 0x1e9, .gaming_fuse = 0x1f5, .highest_bank_bit = 14,
	.gcc_debug_gpucc = 0x146,
	.speed_count = 4, .default_speed = 3,
	.speeds = { {0, 0}, {315000, 64}, {450000, 128}, {550000, 192} },
};
static const struct adreno_board_profile adreno_renoir = {
	.board = "renoir", .model = "Adreno 642", .firmware_directory = "fw-renoir",
	.chip_id = 0x06060010, .gmem_bytes = 1536 * 1024,
	.primitive_fifo = 0x00300000, .hang_cycles = 0x3ffff,
	.speed_fuse = 0x1e1, .gaming_fuse = 0x1ed, .highest_bank_bit = 14,
	.gcc_debug_gpucc = 0x13c, /* debugcc-shima.c: gpu_cc_debug_mux */
	/* Shima speed bin 105 OPPs: 285MHz (LOW_SVS), 365MHz (LOW_SVS_L1), 443MHz (SVS), 490MHz (SVS_L0). Default: 490MHz */
	.speed_count = 5, .default_speed = 4,
	.speeds = { {0, 0}, {285000, 64}, {365000, 80}, {443000, 128}, {490000, 144} },
};

/* A642's patch high nibble encodes target, not patch: GMU expects it in [31:28]. */
static uint32_t
adreno642_gmu_chip_id(uint32_t chip)
{
	return (chip & 0xffff0000) | ((chip >> 8 & 0xff) << 12) |
	       ((chip & 0xf) << 8) | ((chip >> 4 & 0xf) << 28);
}

static const struct adreno_board_profile *
adreno_profile_for_board(const char *board)
{
	if (board && strcmp(board, "redwood") == 0) { return &adreno_redwood; }
	if (board && strcmp(board, "renoir") == 0) { return &adreno_renoir; }
	return NULL;
}
#endif
