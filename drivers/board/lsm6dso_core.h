/*
 * The facts of an ST LSM6DSO/LSM6DSOX inertial chip (a 6-axis accelerometer + gyroscope, common on
 * Snapdragon phones on I2C or SPI), apart from any bus: its register numbers, the identity it answers
 * with, how its output registers assemble into signed samples, and how many counts it gives per g at
 * each full-scale setting. A driver reads the bytes over whatever bus the board wires (board/i2c_gpio.h)
 * and uses this to make sense of them; the numbers here are from the public ST datasheet (DS12306), no
 * board's and no vendor's. Integers only, no state: the host test (workbench/lsm6dso_test.c) compiles
 * this same file, so what is tested is what the driver uses.
 *
 * The chip gives its six outputs little-endian, gyro first: a read of 12 bytes from OUTX_L_G returns
 * gyro x,y,z then accel x,y,z, each a 16-bit two's-complement count. Only the accelerometer drives
 * orientation (orientation_core.h, sensors_core.h); the gyro's rates go to Darwin's motion stack beside the
 * accelerometer's samples (board/imu.c), at the full scale and sensitivity below.
 */
#ifndef _HAKIOS_LSM6DSO_CORE_H
#define _HAKIOS_LSM6DSO_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* registers (ST DS12306) */
#define LSM6DSO_REG_WHO_AM_I   0x0F
#define LSM6DSO_REG_CTRL1_XL   0x10   /* accelerometer: output data rate and full scale */
#define LSM6DSO_REG_CTRL2_G    0x11   /* gyroscope: output data rate and full scale */
#define LSM6DSO_REG_CTRL3_C    0x12   /* BDU, auto-increment, software reset */
#define LSM6DSO_REG_OUTX_L_G   0x22   /* first of the twelve output bytes: gyro x,y,z then accel x,y,z */
#define LSM6DSO_OUTPUT_BYTES   12

#define LSM6DSO_REG_OUTX_L_A   0x28   /* the accelerometer's first: OUTX_L_G's twelve are the gyroscope's six, then these six */

/* WHO_AM_I: 0x6c for the LSM6DSO and LSM6DSOX, which share this register map */
#define LSM6DSO_WHO_AM_I       0x6c

/* CTRL3_C bits */
#define LSM6DSO_CTRL3_SW_RESET 0x01
#define LSM6DSO_CTRL3_IF_INC   0x04   /* auto-increment the register across a multi-byte read */
#define LSM6DSO_CTRL3_BDU      0x40   /* block data update: a sample's bytes do not change mid-read */

/* the accelerometer full-scale settings (CTRL1_XL bits 3:2) and the counts per g each gives */
enum {
	LSM6DSO_FS_2G  = 0,
	LSM6DSO_FS_16G = 1,
	LSM6DSO_FS_4G  = 2,
	LSM6DSO_FS_8G  = 3,
};

/*
 * The gyroscope's full-scale settings (CTRL2_G bits 3:2, FS_125 at bit 1 left 0) and its sensitivity at each, in
 * micro-degrees per second per count: the datasheet's typical G_So, 8.75 / 17.50 / 35 / 70 mdps per LSB at +-250 /
 * 500 / 1000 / 2000 dps -- each wider scale doubles it.
 */
enum {
	LSM6DSO_FS_G_250DPS  = 0,
	LSM6DSO_FS_G_500DPS  = 1,
	LSM6DSO_FS_G_1000DPS = 2,
	LSM6DSO_FS_G_2000DPS = 3,
};

static inline int32_t
lsm6dso_gyro_udps_per_count(int full_scale)
{
	switch (full_scale) {
	case LSM6DSO_FS_G_250DPS:  return 8750;
	case LSM6DSO_FS_G_500DPS:  return 17500;
	case LSM6DSO_FS_G_1000DPS: return 35000;
	case LSM6DSO_FS_G_2000DPS: return 70000;
	default:                   return 0;      /* not a full-scale setting */
	}
}

struct lsm6dso_sample {
	int16_t gyro[3];    /* x, y, z, raw counts */
	int16_t accel[3];   /* x, y, z, raw counts */
};

/* assemble the twelve output bytes (as read from OUTX_L_G) into signed samples */
static inline struct lsm6dso_sample
lsm6dso_decode(const uint8_t out[LSM6DSO_OUTPUT_BYTES])
{
	struct lsm6dso_sample s;
	for (int i = 0; i < 3; i++) {
		s.gyro[i]  = (int16_t)((uint16_t)out[2 * i]     | (uint16_t)out[2 * i + 1] << 8);
		s.accel[i] = (int16_t)((uint16_t)out[6 + 2 * i] | (uint16_t)out[6 + 2 * i + 1] << 8);
	}
	return s;
}

/*
 * Counts per g at a full-scale setting: at +-2g the chip gives 0.061 mg per count, so 1000/0.061 ~=
 * 16384 counts per g; each wider scale halves it. A driver turns a count into milli-g as
 * count * 1000 / counts_per_g (sensor_axes_core.h's scale is exactly {1000, counts_per_g}).
 */
static inline int32_t
lsm6dso_counts_per_g(int full_scale)
{
	switch (full_scale) {
	case LSM6DSO_FS_2G:  return 16384;
	case LSM6DSO_FS_4G:  return 8192;
	case LSM6DSO_FS_8G:  return 4096;
	case LSM6DSO_FS_16G: return 2048;
	default:             return 0;      /* not a full-scale setting */
	}
}

/* CTRL1_XL value for an accelerometer output-data-rate code (bits 7:4) and a full scale (bits 3:2) */
static inline uint8_t
lsm6dso_ctrl1_xl(uint8_t odr, int full_scale)
{
	return (uint8_t)(odr << 4 | ((unsigned)full_scale & 3) << 2);
}

/*
 * CTRL2_G value for a gyroscope output-data-rate code (bits 7:4, the same codes as CTRL1_XL's: 4 is 104 Hz) and a
 * full scale (bits 3:2); bits 1:0 left 0 (FS_125 off, and bit 0 must be). 0 powers the gyroscope down, as a reset
 * leaves it.
 */
static inline uint8_t
lsm6dso_ctrl2_g(uint8_t odr, int full_scale)
{
	return (uint8_t)(odr << 4 | ((unsigned)full_scale & 3) << 2);
}

#endif /* _HAKIOS_LSM6DSO_CORE_H */
