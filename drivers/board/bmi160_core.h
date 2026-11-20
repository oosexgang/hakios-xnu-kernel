/*
 * The facts of a Bosch BMI160 inertial chip (a 6-axis accelerometer + gyroscope, common on Snapdragon phones on
 * I2C or SPI), apart from any bus: its register numbers, the identity it answers with, the commands that switch
 * its two halves on and off, how its output registers assemble into signed samples, and how many counts it gives
 * per g and per degree a second at each range. A driver reads the bytes over whatever bus the board wires
 * (board/i2c_gpio.h) and uses this to make sense of them; the numbers here are the public Bosch datasheet's
 * (BST-BMI160-DS000) as remembered, no board's and no vendor's driver's -- nothing here has been run against a chip,
 * and what is less than sure says so where it stands. Integers only, no state: the host test
 * (workbench/bmi160_test.c) compiles this same file, so what is tested is what the driver uses.
 *
 * Only the BMI160: a BMI260 or BMI270 answers another identity, wants a configuration file loaded before it
 * measures anything and has another register map; neither is this file's.
 *
 * The chip gives its six outputs little-endian, gyro first, as an LSM6DSO does (lsm6dso_core.h): a read of 12 bytes
 * from DATA_8 (0x0c) returns gyro x,y,z then accel x,y,z, each a 16-bit two's-complement count; the register address
 * counts up across a read by itself, and a read that starts at a sample's low byte gets that sample whole (the chip
 * holds the high byte for it). Its axes are right-handed and its accelerometer reads +1 g along the axis that points
 * up, as an LSM6DSO's does, so a board's mount means the same for both (sensor_axes_core.h).
 *
 * What differs from an LSM6DSO is how a half is switched on: not by the rate written into its control register, but
 * by a command written to CMD, which the chip takes some time to carry out -- and drops if it comes while the last
 * one is still being carried out -- and says done in PMU_STATUS. CMD itself does not read back what was written.
 */
#ifndef _HAKIOS_BMI160_CORE_H
#define _HAKIOS_BMI160_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* registers (Bosch BST-BMI160-DS000) */
#define BMI160_REG_CHIP_ID      0x00
#define BMI160_REG_PMU_STATUS   0x03   /* which power mode each half is in: accelerometer bits 5:4, gyroscope bits 3:2 */
#define BMI160_REG_DATA_GYR     0x0c   /* first of the twelve output bytes: gyro x,y,z then accel x,y,z (DATA_8 .. DATA_19) */
#define BMI160_OUTPUT_BYTES     12
#define BMI160_REG_DATA_ACC     0x12   /* the accelerometer's first: DATA_GYR's twelve are the gyroscope's six, then these six */
#define BMI160_REG_ACC_CONF     0x40   /* accelerometer: output data rate (bits 3:0), filter (bits 6:4), undersampling (bit 7) */
#define BMI160_REG_ACC_RANGE    0x41   /* accelerometer: range (bits 3:0) */
#define BMI160_REG_GYR_CONF     0x42   /* gyroscope: output data rate (bits 3:0), filter (bits 5:4) */
#define BMI160_REG_GYR_RANGE    0x43   /* gyroscope: range (bits 2:0) */
#define BMI160_REG_CMD          0x7e   /* a command: a half's power mode, a reset */

/* CHIP_ID: 0xd1 for the BMI160 */
#define BMI160_CHIP_ID          0xd1

/* the chip's two I2C addresses, by how its SDO pin is strapped: low 0x68, high 0x69 */
#define BMI160_ADDRESS_SDO_LOW  0x68
#define BMI160_ADDRESS_SDO_HIGH 0x69

/* commands (CMD) */
#define BMI160_CMD_ACC_SUSPEND  0x10   /* the accelerometer off */
#define BMI160_CMD_ACC_NORMAL   0x11   /* ... measuring */
#define BMI160_CMD_GYR_SUSPEND  0x14   /* the gyroscope off */
#define BMI160_CMD_GYR_NORMAL   0x15   /* ... measuring */
#define BMI160_CMD_SOFTRESET    0xb6   /* everything as power-on leaves it: both halves suspended, the registers at their defaults */

/* a half's power mode as PMU_STATUS says it (two bits each) */
enum {
	BMI160_PMU_SUSPEND = 0,
	BMI160_PMU_NORMAL  = 1,
	/* (2 is the accelerometer's low power, 3 the gyroscope's fast start-up: not used) */
};

static inline unsigned
bmi160_acc_pmu(uint8_t pmu_status)
{
	return (pmu_status >> 4) & 3;
}

static inline unsigned
bmi160_gyr_pmu(uint8_t pmu_status)
{
	return (pmu_status >> 2) & 3;
}

/*
 * How long the chip takes, in milliseconds, each with room: a driver looks at PMU_STATUS (or, after a reset, for
 * the identity) until the chip says it is done or this long has passed, rather than trust the figure. The
 * datasheet's, as remembered and NOT SURE to the millisecond: the accelerometer measures some 3.8 ms after its
 * command, the gyroscope 55 ms typically and 80 ms at most after its (it has a mass to set swinging), and the
 * chip is back from a reset within about 10 ms. A half is suspended sooner than it starts.
 */
#define BMI160_ACC_START_MS     20
#define BMI160_GYR_START_MS     150
#define BMI160_RESET_MS         50
#define BMI160_SUSPEND_MS       20
/*
 * ... and between two writes while the chip is not measuring (both halves suspended) it wants 450 microseconds
 * (2 microseconds while it is): a driver that waits a millisecond after a write is on the safe side of both.
 */
#define BMI160_WRITE_GAP_MS     1

/* the output-data-rate codes ACC_CONF and GYR_CONF share (bits 3:0): 100 Hz * 2^(code - 8); the reset's is 8 */
#define BMI160_ODR_25_HZ        6
#define BMI160_ODR_50_HZ        7
#define BMI160_ODR_100_HZ       8
#define BMI160_ODR_200_HZ       9

/*
 * ACC_CONF for an output-data-rate code: the filter's normal mode (acc_bwp 2 in bits 6:4), no undersampling (bit 7
 * 0). At 100 Hz that is 0x28, which is also what a reset leaves.
 */
static inline uint8_t
bmi160_acc_conf(uint8_t odr)
{
	return (uint8_t)(2u << 4 | (odr & 0x0f));
}

/* GYR_CONF likewise: the filter's normal mode (gyr_bwp 2 in bits 5:4); 0x28 at 100 Hz, the reset's too */
static inline uint8_t
bmi160_gyr_conf(uint8_t odr)
{
	return (uint8_t)(2u << 4 | (odr & 0x0f));
}

/* the accelerometer's ranges as ACC_RANGE has them (bits 3:0; not a count: these four codes and no others) */
enum {
	BMI160_ACC_RANGE_2G  = 0x03,
	BMI160_ACC_RANGE_4G  = 0x05,
	BMI160_ACC_RANGE_8G  = 0x08,
	BMI160_ACC_RANGE_16G = 0x0c,
};

/* ACC_RANGE's code for a range in g (2, 4, 8, 16), or 0 -- which is no range's code -- for any other */
static inline uint8_t
bmi160_acc_range(unsigned g)
{
	switch (g) {
	case 2:  return BMI160_ACC_RANGE_2G;
	case 4:  return BMI160_ACC_RANGE_4G;
	case 8:  return BMI160_ACC_RANGE_8G;
	case 16: return BMI160_ACC_RANGE_16G;
	default: return 0;
	}
}

/*
 * Counts per g at a range: 16384 at +-2 g (the sixteen bits spread over the 4 g), each wider range halves it. A
 * driver turns a count into milli-g as count * 1000 / counts_per_g (sensor_axes_core.h's scale is exactly
 * {1000, counts_per_g}).
 */
static inline int32_t
bmi160_counts_per_g(int range)
{
	switch (range) {
	case BMI160_ACC_RANGE_2G:  return 16384;
	case BMI160_ACC_RANGE_4G:  return 8192;
	case BMI160_ACC_RANGE_8G:  return 4096;
	case BMI160_ACC_RANGE_16G: return 2048;
	default:                   return 0;      /* not a range's code */
	}
}

/*
 * The gyroscope's ranges as GYR_RANGE has them (bits 2:0) -- the widest is 0, what a reset leaves, the other way
 * round from an LSM6DSO -- and its sensitivity at each, in micro-degrees per second per count. The datasheet gives
 * it as counts per degree a second: 16.4 / 32.8 / 65.6 / 131.2 / 262.4 at +-2000 / 1000 / 500 / 250 / 125 dps, each
 * narrower range doubling it; a count is then 1000000 / 16.4 = 60976 micro-dps at +-2000 dps (rounded to the
 * nearest; the sixteen bits spread evenly over the 4000 dps would be 61035, a thousandth more, far inside the
 * chip's own spread), and half of the one before at each narrower range.
 */
enum {
	BMI160_GYR_RANGE_2000DPS = 0,
	BMI160_GYR_RANGE_1000DPS = 1,
	BMI160_GYR_RANGE_500DPS  = 2,
	BMI160_GYR_RANGE_250DPS  = 3,
	BMI160_GYR_RANGE_125DPS  = 4,
};

static inline int32_t
bmi160_gyro_udps_per_count(int range)
{
	switch (range) {
	case BMI160_GYR_RANGE_2000DPS: return 60976;
	case BMI160_GYR_RANGE_1000DPS: return 30488;
	case BMI160_GYR_RANGE_500DPS:  return 15244;
	case BMI160_GYR_RANGE_250DPS:  return 7622;
	case BMI160_GYR_RANGE_125DPS:  return 3811;
	default:                       return 0;      /* not a range's code */
	}
}

struct bmi160_sample {
	int16_t gyro[3];    /* x, y, z, raw counts */
	int16_t accel[3];   /* x, y, z, raw counts */
};

/* assemble the twelve output bytes (as read from DATA_GYR) into signed samples */
static inline struct bmi160_sample
bmi160_decode(const uint8_t out[BMI160_OUTPUT_BYTES])
{
	struct bmi160_sample s;
	for (int i = 0; i < 3; i++) {
		s.gyro[i]  = (int16_t)((uint16_t)out[2 * i]     | (uint16_t)out[2 * i + 1] << 8);
		s.accel[i] = (int16_t)((uint16_t)out[6 + 2 * i] | (uint16_t)out[6 + 2 * i + 1] << 8);
	}
	return s;
}

#endif /* _HAKIOS_BMI160_CORE_H */
