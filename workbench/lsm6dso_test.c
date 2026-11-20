/*
 * Host tests of the LSM6DSO chip facts (drivers/board/lsm6dso_core.h): the output bytes assemble into the
 * right signed samples, the counts-per-g table, the gyroscope's sensitivities and the control-register packing are
 * the datasheet's.
 * workbench/run.sh builds this against the kernel's own header.
 */
#include <stdio.h>

#include "board/lsm6dso_core.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static void
test_the_output_bytes_assemble_little_endian_two_s_complement(void)
{
	/* gyro (1, -1, 256), then accel (+16384, -16384, 1), each a little-endian int16 */
	uint8_t out[LSM6DSO_OUTPUT_BYTES] = {
		0x01, 0x00,  0xFF, 0xFF,  0x00, 0x01,   /* gyro x, y, z */
		0x00, 0x40,  0x00, 0xC0,  0x01, 0x00,   /* accel x, y, z */
	};
	struct lsm6dso_sample s = lsm6dso_decode(out);
	CHECK(s.gyro[0] == 1, "gyro x: %d", s.gyro[0]);
	CHECK(s.gyro[1] == -1, "gyro y (0xffff): %d", s.gyro[1]);
	CHECK(s.gyro[2] == 256, "gyro z: %d", s.gyro[2]);
	CHECK(s.accel[0] == 16384, "accel x: %d", s.accel[0]);
	CHECK(s.accel[1] == -16384, "accel y (0xc000): %d", s.accel[1]);
	CHECK(s.accel[2] == 1, "accel z: %d", s.accel[2]);
}

static void
test_counts_per_g_halves_with_each_wider_scale(void)
{
	CHECK(lsm6dso_counts_per_g(LSM6DSO_FS_2G) == 16384, "2g");
	CHECK(lsm6dso_counts_per_g(LSM6DSO_FS_4G) == 8192, "4g");
	CHECK(lsm6dso_counts_per_g(LSM6DSO_FS_8G) == 4096, "8g");
	CHECK(lsm6dso_counts_per_g(LSM6DSO_FS_16G) == 2048, "16g");
	CHECK(lsm6dso_counts_per_g(99) == 0, "an unknown full scale is refused");
}

static void
test_the_identity_and_control_packing(void)
{
	CHECK(LSM6DSO_WHO_AM_I == 0x6c, "who_am_i");
	/* ODR code 4 (104 Hz) at +-4g: bits 7:4 = 4, bits 3:2 = 2 -> 0x48 */
	CHECK(lsm6dso_ctrl1_xl(4, LSM6DSO_FS_4G) == 0x48, "ctrl1_xl: %#x", lsm6dso_ctrl1_xl(4, LSM6DSO_FS_4G));
}

static void
test_the_gyroscope_s_facts(void)
{
	/* the twelve output bytes are contiguous: the gyroscope's six from 0x22, the accelerometer's six from 0x28 to 0x2d */
	CHECK(LSM6DSO_REG_OUTX_L_G == 0x22 && LSM6DSO_REG_OUTX_L_A == LSM6DSO_REG_OUTX_L_G + 6 &&
	    LSM6DSO_REG_OUTX_L_G + LSM6DSO_OUTPUT_BYTES - 1 == 0x2d, "the burst is not OUTX_L_G .. OUTZ_H_A");
	CHECK(LSM6DSO_REG_CTRL2_G == 0x11, "ctrl2_g");
	/* the datasheet's G_So: 8.75, 17.5, 35, 70 mdps per count at +-250, 500, 1000, 2000 dps */
	CHECK(lsm6dso_gyro_udps_per_count(LSM6DSO_FS_G_250DPS) == 8750, "250 dps");
	CHECK(lsm6dso_gyro_udps_per_count(LSM6DSO_FS_G_500DPS) == 17500, "500 dps");
	CHECK(lsm6dso_gyro_udps_per_count(LSM6DSO_FS_G_1000DPS) == 35000, "1000 dps");
	CHECK(lsm6dso_gyro_udps_per_count(LSM6DSO_FS_G_2000DPS) == 70000, "2000 dps");
	CHECK(lsm6dso_gyro_udps_per_count(7) == 0, "an unknown full scale is refused");
	/* ODR code 4 (104 Hz) at +-2000 dps: bits 7:4 = 4, bits 3:2 = 3 (FS_G 11), bits 1:0 0 -> 0x4c */
	CHECK(lsm6dso_ctrl2_g(4, LSM6DSO_FS_G_2000DPS) == 0x4c, "ctrl2_g: %#x", lsm6dso_ctrl2_g(4, LSM6DSO_FS_G_2000DPS));
	CHECK(lsm6dso_ctrl2_g(4, LSM6DSO_FS_G_250DPS) == 0x40, "ctrl2_g at 250 dps: %#x", lsm6dso_ctrl2_g(4, LSM6DSO_FS_G_250DPS));
}

int
main(void)
{
	test_the_output_bytes_assemble_little_endian_two_s_complement();
	test_counts_per_g_halves_with_each_wider_scale();
	test_the_identity_and_control_packing();
	test_the_gyroscope_s_facts();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
