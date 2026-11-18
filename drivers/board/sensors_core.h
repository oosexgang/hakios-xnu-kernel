/*
 * The motion-sensor pipeline, apart from any hardware: the one seam a sensor-hub driver calls. It holds a
 * board's accelerometer mounting and scale (sensor_axes_core.h) and an orientation detector
 * (orientation_core.h), and turns a raw accelerometer sample from the chip into the current
 * UIDeviceOrientation in one call -- chip frame and counts in, Darwin's orientation out. Integers only, the
 * state in a struct the caller holds: the same file is compiled by the host test (workbench/sensors_test.c).
 *
 * A hub driver fills `accel.mount` and `accel.scale` from the board's sensor description once, calls
 * sensors_accel_reset(), then sensors_accel_raw() for each sample the hub delivers; what it returns is what
 * auto-rotate reads. The gyroscope and magnetometer, when a later piece feeds Darwin device motion, scale
 * through sensor_to_darwin() the same way; only the accelerometer drives orientation, so only it is here.
 */
#ifndef _HAKIOS_SENSORS_CORE_H
#define _HAKIOS_SENSORS_CORE_H

#include "sensor_axes_core.h"
#include "orientation_core.h"

struct sensors_accel {
	struct sensor_mount    mount;    /* the chip's mounting on this board (chip axes -> Darwin axes) */
	struct sensor_scale    scale;    /* the chip's counts -> milli-g */
	struct orient_detector orient;   /* the orientation the samples have settled on */
};

static inline void
sensors_accel_reset(struct sensors_accel *s)
{
	orient_reset(&s->orient);
}

/*
 * One raw accelerometer sample, in the chip's own axes and counts. Returns the current orientation (an
 * ORIENT_* value). The sample is turned onto Darwin's axes and scaled to milli-g before the detector sees
 * it, so a chip mounted turned or flipped reads the right way up.
 */
static inline int
sensors_accel_raw(struct sensors_accel *s, int32_t rx, int32_t ry, int32_t rz)
{
	struct sensor_sample milli_g;

	milli_g = sensor_to_darwin(&s->mount, &s->scale, (struct sensor_sample){ { rx, ry, rz } });
	return orient_update(&s->orient, milli_g.v[0], milli_g.v[1], milli_g.v[2]);
}

#endif /* _HAKIOS_SENSORS_CORE_H */
