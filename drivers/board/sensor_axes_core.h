/*
 * The arithmetic between a motion-sensor chip and the orientation detector, apart from any hardware: the
 * chip is mounted on each board turned and perhaps flipped, and counts in its own units, so its raw
 * samples are turned onto Darwin's axes (orientation_core.h) and scaled to Darwin's units before anything
 * reads them. Which turn and which scale is the board's (a sensor chip's mounting, its counts per g / per
 * degree / per gauss), not this file's. Integers only (the kernel has no floating point), no state: the
 * same file is compiled by the host test (workbench/sensor_axes_test.c), so what is tested is what runs.
 *
 * Darwin's axes and units are orientation_core.h's: +x right, +y up (away from the home button), +z out of
 * the screen; milli-g for the accelerometer, milli-rad/s for the gyroscope, milli-uT (= milli-gauss / 10)
 * for the magnetometer.
 *
 * A mounting is a signed permutation: each Darwin axis is plus or minus one of the chip's axes. `axis[i]`
 * is which chip axis (0, 1, 2) Darwin's axis i comes from, `sign[i]` its sign (+1 or -1). The identity
 * mounting is axis {0,1,2}, sign {1,1,1}. A scale turns the chip's counts into Darwin's unit as
 * num/den, so a chip of 16384 counts per g has accel num=1000, den=16384 (count * 1000 / 16384 = milli-g).
 */
#ifndef _HAKIOS_SENSOR_AXES_CORE_H
#define _HAKIOS_SENSOR_AXES_CORE_H

#include <stdint.h>
#include <stdbool.h>

struct sensor_sample {
	int32_t v[3];   /* x, y, z */
};

struct sensor_mount {
	uint8_t axis[3];   /* Darwin axis i reads chip axis axis[i] (each 0, 1 or 2) */
	int8_t  sign[3];   /* ... with this sign (+1 or -1) */
};

struct sensor_scale {
	int32_t num, den;   /* a chip count becomes count * num / den in Darwin's unit; den must not be 0 */
};

/* Is MOUNT a real signed permutation: each of axis[] a different one of 0,1,2, each sign +1 or -1. A board
 * file that gives a bad mounting is caught here, not by the samples coming out wrong. */
static inline bool
sensor_mount_ok(const struct sensor_mount *m)
{
	bool seen[3] = { false, false, false };
	for (int i = 0; i < 3; i++) {
		if (m->axis[i] > 2 || seen[m->axis[i]]) {
			return false;
		}
		seen[m->axis[i]] = true;
		if (m->sign[i] != 1 && m->sign[i] != -1) {
			return false;
		}
	}
	return true;
}

/* Turn a raw chip sample onto Darwin's axes (the mounting) and into Darwin's unit (the scale), rounding to
 * the nearest, halves away from zero. The scale is applied in 64 bits so a large count cannot overflow. */
static inline struct sensor_sample
sensor_to_darwin(const struct sensor_mount *m, const struct sensor_scale *s, struct sensor_sample raw)
{
	struct sensor_sample out;
	for (int i = 0; i < 3; i++) {
		int64_t count = (int64_t)m->sign[i] * raw.v[m->axis[i]];
		int64_t scaled = count * s->num;
		int64_t half = (scaled < 0 ? -s->den : s->den) / 2;
		out.v[i] = (int32_t)((scaled + half) / s->den);
	}
	return out;
}

/*
 * A gyroscope on the same chip as an accelerometer is turned on the board as the accelerometer is -- its axes are
 * the chip's same x, y and z -- but an accelerometer's mounting carries one more turn: the chip reads the reaction to
 * gravity (+1 g up) where Darwin wants gravity itself (-1 g down, orientation_core.h), so a board's accelerometer
 * mounting is the chip's true turn with every sign the other way. A rotation rate has no such turn: its mounting is
 * the accelerometer's with every sign turned back. Both frames are right-handed (the chip's datasheet draws its axes
 * so, and Darwin's are), so the true turn is a rotation, and an accelerometer's mounting, that with every sign turned, a
 * reflection: a board whose mounting is not one (determinant +1) says a turn no right-handed chip can have, and the
 * gyroscope is not given rather than given with its rates the wrong way round. True and the gyroscope's mounting in
 * GYRO, or false.
 */
static inline bool
sensor_gyro_mount(const struct sensor_mount *accel, struct sensor_mount *gyro)
{
	int determinant = 1;

	if (!sensor_mount_ok(accel)) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		determinant *= accel->sign[i];
		for (int j = i + 1; j < 3; j++) {
			determinant *= accel->axis[i] > accel->axis[j] ? -1 : 1;  /* (each pair out of order is a swap) */
		}
	}
	if (determinant != -1) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		gyro->axis[i] = accel->axis[i];
		gyro->sign[i] = (int8_t)-accel->sign[i];
	}
	return true;
}

/*
 * The scale that turns a gyroscope's counts into milli-rad/s, from its sensitivity in micro-degrees per second per
 * count: count * udps / 1000 is milli-degrees per second, times pi / 180 milli-radians -- pi taken as 355/113, which
 * is off by less than one part in ten million, far below a gyroscope's own spread.
 */
static inline struct sensor_scale
sensor_gyro_scale(int32_t udps_per_count)
{
	return (struct sensor_scale){ udps_per_count * 355, 180 * 113 * 1000 };
}

#endif /* _HAKIOS_SENSOR_AXES_CORE_H */
