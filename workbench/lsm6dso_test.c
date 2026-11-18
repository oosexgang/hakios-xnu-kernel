/*
 * Host tests of the LSM6DSO chip facts (drivers/board/lsm6dso_core.h): the output bytes assemble into the
 * right signed samples, the counts-per-g table and the control-register packing are the datasheet's.
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

int
main(void)
{
	test_the_output_bytes_assemble_little_endian_two_s_complement();
	test_counts_per_g_halves_with_each_wider_scale();
	test_the_identity_and_control_packing();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
