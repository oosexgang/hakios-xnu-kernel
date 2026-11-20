/*
 * Host tests of the BMI160 chip facts (drivers/board/bmi160_core.h): the output bytes assemble into the right
 * signed samples, the register numbers and commands, the range codes with their counts per g, the gyroscope's
 * sensitivities, the rate packing and the reading of PMU_STATUS are the datasheet's, as that file remembers it.
 * workbench/run.sh builds this against the kernel's own header.
 */
#include <stdio.h>

#include "board/bmi160_core.h"

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
	uint8_t out[BMI160_OUTPUT_BYTES] = {
		0x01, 0x00,  0xFF, 0xFF,  0x00, 0x01,   /* gyro x, y, z */
		0x00, 0x40,  0x00, 0xC0,  0x01, 0x00,   /* accel x, y, z */
	};
	struct bmi160_sample s = bmi160_decode(out);
	CHECK(s.gyro[0] == 1, "gyro x: %d", s.gyro[0]);
	CHECK(s.gyro[1] == -1, "gyro y (0xffff): %d", s.gyro[1]);
	CHECK(s.gyro[2] == 256, "gyro z: %d", s.gyro[2]);
	CHECK(s.accel[0] == 16384, "accel x: %d", s.accel[0]);
	CHECK(s.accel[1] == -16384, "accel y (0xc000): %d", s.accel[1]);
	CHECK(s.accel[2] == 1, "accel z: %d", s.accel[2]);
	/* the ends of the range, and each byte in its own place */
	uint8_t ends[BMI160_OUTPUT_BYTES] = { 0x00, 0x80,  0xFF, 0x7F,  0x34, 0x12,  0xFF, 0x7F,  0x00, 0x80,  0xCD, 0xAB };
	s = bmi160_decode(ends);
	CHECK(s.gyro[0] == -32768 && s.gyro[1] == 32767 && s.gyro[2] == 0x1234, "gyro: %d %d %d", s.gyro[0], s.gyro[1], s.gyro[2]);
	CHECK(s.accel[0] == 32767 && s.accel[1] == -32768 && s.accel[2] == (int16_t)0xabcd, "accel: %d %d %d", s.accel[0], s.accel[1],
	    s.accel[2]);
}

static void
test_the_registers_the_identity_and_the_commands(void)
{
	CHECK(BMI160_REG_CHIP_ID == 0x00 && BMI160_CHIP_ID == 0xd1, "chip id");
	CHECK(BMI160_REG_PMU_STATUS == 0x03, "pmu_status");
	/* the twelve output bytes are contiguous: the gyroscope's six from 0x0c, the accelerometer's six from 0x12 to 0x17 */
	CHECK(BMI160_REG_DATA_GYR == 0x0c && BMI160_REG_DATA_ACC == BMI160_REG_DATA_GYR + 6 &&
	    BMI160_REG_DATA_GYR + BMI160_OUTPUT_BYTES - 1 == 0x17, "the burst is not DATA_8 .. DATA_19");
	CHECK(BMI160_REG_ACC_CONF == 0x40 && BMI160_REG_ACC_RANGE == 0x41 && BMI160_REG_GYR_CONF == 0x42 &&
	    BMI160_REG_GYR_RANGE == 0x43, "the rate and range registers");
	CHECK(BMI160_REG_CMD == 0x7e, "cmd");
	CHECK(BMI160_CMD_ACC_SUSPEND == 0x10 && BMI160_CMD_ACC_NORMAL == 0x11, "the accelerometer's commands");
	CHECK(BMI160_CMD_GYR_SUSPEND == 0x14 && BMI160_CMD_GYR_NORMAL == 0x15, "the gyroscope's commands");
	CHECK(BMI160_CMD_SOFTRESET == 0xb6, "the reset");
	CHECK(BMI160_ADDRESS_SDO_LOW == 0x68 && BMI160_ADDRESS_SDO_HIGH == 0x69, "the addresses");
}

static void
test_pmu_status_says_each_half_s_mode(void)
{
	/* accelerometer bits 5:4, gyroscope bits 3:2; the magnetometer interface's bits 1:0 are nobody's here */
	CHECK(bmi160_acc_pmu(0x00) == BMI160_PMU_SUSPEND && bmi160_gyr_pmu(0x00) == BMI160_PMU_SUSPEND, "both suspended");
	CHECK(bmi160_acc_pmu(0x10) == BMI160_PMU_NORMAL && bmi160_gyr_pmu(0x10) == BMI160_PMU_SUSPEND, "the accelerometer alone");
	CHECK(bmi160_acc_pmu(0x04) == BMI160_PMU_SUSPEND && bmi160_gyr_pmu(0x04) == BMI160_PMU_NORMAL, "the gyroscope alone");
	CHECK(bmi160_acc_pmu(0x14) == BMI160_PMU_NORMAL && bmi160_gyr_pmu(0x14) == BMI160_PMU_NORMAL, "both");
	CHECK(bmi160_acc_pmu(0x2c) == 2 && bmi160_gyr_pmu(0x2c) == 3, "low power and fast start-up are neither: %u %u",
	    bmi160_acc_pmu(0x2c), bmi160_gyr_pmu(0x2c));
	CHECK(bmi160_acc_pmu(0xc3) == BMI160_PMU_SUSPEND && bmi160_gyr_pmu(0xc3) == BMI160_PMU_SUSPEND, "the other bits are not looked at");
}

static void
test_the_accelerometer_s_ranges(void)
{
	CHECK(bmi160_acc_range(2) == 0x03 && bmi160_acc_range(4) == 0x05 && bmi160_acc_range(8) == 0x08 && bmi160_acc_range(16) == 0x0c,
	    "the four codes: %#x %#x %#x %#x", bmi160_acc_range(2), bmi160_acc_range(4), bmi160_acc_range(8), bmi160_acc_range(16));
	CHECK(bmi160_acc_range(0) == 0 && bmi160_acc_range(3) == 0 && bmi160_acc_range(32) == 0, "no other range has a code");
	CHECK(bmi160_counts_per_g(BMI160_ACC_RANGE_2G) == 16384, "2g");
	CHECK(bmi160_counts_per_g(BMI160_ACC_RANGE_4G) == 8192, "4g");
	CHECK(bmi160_counts_per_g(BMI160_ACC_RANGE_8G) == 4096, "8g");
	CHECK(bmi160_counts_per_g(BMI160_ACC_RANGE_16G) == 2048, "16g");
	/* a range in g is not its code: 2 and 4 are no codes, nor is 0 */
	CHECK(bmi160_counts_per_g(0) == 0 && bmi160_counts_per_g(2) == 0 && bmi160_counts_per_g(4) == 0, "an unknown code is refused");
	/* the sixteen bits spread over the range: counts per g times the range's width is 65536 */
	for (unsigned g = 2; g <= 16; g *= 2) {
		CHECK(bmi160_counts_per_g(bmi160_acc_range(g)) * 2 * (int32_t)g == 65536, "+-%u g", g);
	}
}

static void
test_the_gyroscope_s_ranges(void)
{
	/* the datasheet's 16.4, 32.8, 65.6, 131.2, 262.4 counts per dps, as micro-dps a count */
	CHECK(bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_2000DPS) == 60976, "2000 dps");
	CHECK(bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_1000DPS) == 30488, "1000 dps");
	CHECK(bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_500DPS) == 15244, "500 dps");
	CHECK(bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_250DPS) == 7622, "250 dps");
	CHECK(bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_125DPS) == 3811, "125 dps");
	CHECK(bmi160_gyro_udps_per_count(5) == 0 && bmi160_gyro_udps_per_count(-1) == 0, "an unknown code is refused");
	CHECK(BMI160_GYR_RANGE_2000DPS == 0 && BMI160_GYR_RANGE_125DPS == 4, "the widest is 0, the reset's");
	/* 16.4 counts a dps: 164 counts are 10 dps, to the micro-dps a count the table keeps */
	CHECK(164 * (int64_t)bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_2000DPS) / 1000 == 10000, "164 counts at 2000 dps: %lld mdps",
	    (long long)(164 * (int64_t)bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_2000DPS) / 1000));
	CHECK(2624 * (int64_t)bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_125DPS) / 1000 == 10000, "2624 counts at 125 dps: %lld mdps",
	    (long long)(2624 * (int64_t)bmi160_gyro_udps_per_count(BMI160_GYR_RANGE_125DPS) / 1000));
}

static void
test_the_rate_packing(void)
{
	/* 100 Hz is code 8; with the normal filter (2 in bits 6:4, or 5:4) both are 0x28, which a reset leaves too */
	CHECK(BMI160_ODR_100_HZ == 8 && BMI160_ODR_25_HZ == 6 && BMI160_ODR_50_HZ == 7 && BMI160_ODR_200_HZ == 9, "the rate codes");
	CHECK(bmi160_acc_conf(BMI160_ODR_100_HZ) == 0x28, "acc_conf: %#x", bmi160_acc_conf(BMI160_ODR_100_HZ));
	CHECK(bmi160_gyr_conf(BMI160_ODR_100_HZ) == 0x28, "gyr_conf: %#x", bmi160_gyr_conf(BMI160_ODR_100_HZ));
	CHECK(bmi160_acc_conf(BMI160_ODR_25_HZ) == 0x26 && bmi160_gyr_conf(BMI160_ODR_200_HZ) == 0x29, "other rates");
	CHECK((bmi160_acc_conf(0xff) & 0x80) == 0 && (bmi160_gyr_conf(0xff) & 0xc0) == 0, "undersampling and the bits above stay 0");
}

static void
test_the_waits_have_room(void)
{
	/* the datasheet's 3.8 ms, 80 ms at most and about 10 ms, as remembered: each wait is longer */
	CHECK(BMI160_ACC_START_MS >= 2 * 4 && BMI160_GYR_START_MS >= 80 + 40 && BMI160_RESET_MS >= 2 * 10 && BMI160_SUSPEND_MS >= 4,
	    "the waits");
	CHECK(BMI160_WRITE_GAP_MS * 1000 >= 450, "450 microseconds between writes");
}

int
main(void)
{
	test_the_output_bytes_assemble_little_endian_two_s_complement();
	test_the_registers_the_identity_and_the_commands();
	test_pmu_status_says_each_half_s_mode();
	test_the_accelerometer_s_ranges();
	test_the_gyroscope_s_ranges();
	test_the_rate_packing();
	test_the_waits_have_room();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
