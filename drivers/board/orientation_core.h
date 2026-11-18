/*
 * The arithmetic of device orientation, apart from any hardware: the accelerometer says where gravity
 * pulls, and this turns a stream of those samples into the orientation Darwin's auto-rotate reads
 * (UIDeviceOrientation). A sensor-hub driver feeds it samples; which hub (Snapdragon's SLPI, another
 * phone's) and how they are read is the driver's, not this file's. Integers only (the kernel has no
 * floating point), the state in a struct the caller holds, nothing of the kernel's: the same file is
 * compiled by the host test (workbench/orientation_test.c), so what is tested there is what runs here.
 *
 * Units and axes:
 *   milli-g    the sample's and gravity's unit: 1000 is one g (9.81 m/s^2). The accelerometer reads the
 *              reaction to gravity, so a phone held still, screen up on a table, reads about (0, 0, -1000).
 *   axes       Darwin's: +x out of the right edge, +y out of the top (away from the home button), +z out
 *              of the screen toward the user. So gravity points along:
 *                portrait            (home button down)   -> g.y about -1000
 *                portrait upside-down                     -> g.y about +1000
 *                landscape left      (home button right)  -> g.x about -1000
 *                landscape right     (home button left)   -> g.x about +1000
 *                face up             (screen up)          -> g.z about -1000
 *                face down           (screen down)        -> g.z about +1000
 *
 * A single sample is noisy and may hold more than gravity (the phone is moved), so gravity is a low-pass
 * of the samples, and the orientation only changes when one axis clearly dominates -- a dead zone around
 * the diagonals, so a phone held at an angle does not flap between two orientations.
 */
#ifndef _HAKIOS_ORIENTATION_CORE_H
#define _HAKIOS_ORIENTATION_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* Darwin's UIDeviceOrientation, the numbers Darwin uses. */
enum {
	ORIENT_UNKNOWN              = 0,
	ORIENT_PORTRAIT             = 1,
	ORIENT_PORTRAIT_UPSIDE_DOWN = 2,
	ORIENT_LANDSCAPE_LEFT       = 3,
	ORIENT_LANDSCAPE_RIGHT      = 4,
	ORIENT_FACE_UP              = 5,
	ORIENT_FACE_DOWN            = 6,
};

/* How fast gravity follows the samples: g += (sample - g) >> ORIENT_LP_SHIFT each update. 3 is a fifth of
 * a second to settle at the hub's usual 50 Hz -- slow enough to ignore a hand's shake, quick enough that a
 * turn is caught within a rotation animation. */
#define ORIENT_LP_SHIFT        3

/* A phone this far from flat (the horizontal pull this strong, in milli-g) is held to be in a portrait or
 * landscape orientation; below it, with gravity mostly along z, it is face up or face down. sin(35 deg) is
 * about 574: past about 35 degrees from flat the edge orientations begin. */
#define ORIENT_FLAT_PULL       574

/* The dominant horizontal axis must beat the other by this much (milli-g) to name the orientation; within
 * it the orientation is held, so the diagonal between two does not flap. */
#define ORIENT_MARGIN          180

struct orient_detector {
	int32_t  gx, gy, gz;   /* the low-pass gravity estimate, milli-g */
	int      current;      /* the orientation last settled on (an ORIENT_* value) */
	bool     primed;       /* has a first sample seeded gravity yet */
};

static inline void
orient_reset(struct orient_detector *d)
{
	d->gx = d->gy = d->gz = 0;
	d->current = ORIENT_UNKNOWN;
	d->primed = false;
}

static inline int32_t
orient_abs(int32_t v)
{
	return v < 0 ? -v : v;
}

/*
 * One accelerometer sample (milli-g). Returns the current orientation (an ORIENT_* value): the one just
 * named, or the one held when no axis dominates. The first sample seeds gravity so the detector settles
 * from where the phone is, not from flat.
 */
static inline int
orient_update(struct orient_detector *d, int32_t ax, int32_t ay, int32_t az)
{
	int32_t hx, hy;
	int candidate;

	if (!d->primed) {
		d->gx = ax;
		d->gy = ay;
		d->gz = az;
		d->primed = true;
	} else {
		d->gx += (ax - d->gx) >> ORIENT_LP_SHIFT;
		d->gy += (ay - d->gy) >> ORIENT_LP_SHIFT;
		d->gz += (az - d->gz) >> ORIENT_LP_SHIFT;
	}

	hx = orient_abs(d->gx);
	hy = orient_abs(d->gy);

	/* mostly flat: gravity along z and neither horizontal pull strong -> face up or face down */
	if (hx < ORIENT_FLAT_PULL && hy < ORIENT_FLAT_PULL) {
		candidate = d->gz < 0 ? ORIENT_FACE_UP : ORIENT_FACE_DOWN;
	} else if (hx > hy + ORIENT_MARGIN) {
		candidate = d->gx < 0 ? ORIENT_LANDSCAPE_LEFT : ORIENT_LANDSCAPE_RIGHT;
	} else if (hy > hx + ORIENT_MARGIN) {
		candidate = d->gy < 0 ? ORIENT_PORTRAIT : ORIENT_PORTRAIT_UPSIDE_DOWN;
	} else {
		/* on the diagonal between two edge orientations: keep the one held, so it does not flap */
		candidate = d->current;
	}

	if (candidate != ORIENT_UNKNOWN) {
		d->current = candidate;
	}
	return d->current;
}

#endif /* _HAKIOS_ORIENTATION_CORE_H */
