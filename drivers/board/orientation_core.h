/*
 * The arithmetic of device orientation, apart from any hardware: the accelerometer says where gravity
 * pulls, and this turns a stream of those samples into the orientation Darwin's auto-rotate reads
 * (UIDeviceOrientation). A sensor-hub driver feeds it samples; which hub (Snapdragon's SLPI, another
 * phone's) and how they are read is the driver's, not this file's. Integers only (the kernel has no
 * floating point), the state in a struct the caller holds, nothing of the kernel's: the same file is
 * compiled by the host test (workbench/orientation_test.c), so what is tested there is what runs here.
 *
 * Units and axes:
 *   milli-g    the sample's and gravity's unit: 1000 is one g (9.81 m/s^2). The samples here are gravity as
 *              Darwin's motion APIs give it (the board's mount turns the chip's reading, which is the reaction
 *              to gravity, +1 g up, into that), so a phone held still, screen up on a table, is about (0, 0, -1000).
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

/* ... with this much (milli-g, about 4 degrees) either side of it held: a phone that is face up only goes to an
 * edge orientation past ORIENT_FLAT_PULL + this, and one on an edge only goes flat below ORIENT_FLAT_PULL - this,
 * so a phone held at about 35 degrees, swaying a little, does not flap between face up and portrait. */
#define ORIENT_FLAT_HOLD       60

/* A sample whose pull is not about one g (in milli-g, its length below the one or above the other) is not
 * gravity: a chip that reads all zeros (its first output before a sample is ready, a chip that is stuck or
 * powered down, a phone in free fall) or saturates, or a phone being shaken. It is not taken into the gravity
 * estimate at all, so the orientation held is kept (and a detector not primed yet stays not known). */
#define ORIENT_G_MIN           500
#define ORIENT_G_MAX           1500

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
 * named, or the one held when no axis dominates or the sample is not gravity. The first sample that is gravity
 * seeds the estimate so the detector settles from where the phone is, not from flat.
 */
static inline int
orient_update(struct orient_detector *d, int32_t ax, int32_t ay, int32_t az)
{
	int32_t hx, hy, flat_limit;
	int64_t length2;
	int candidate;

	/* not gravity (ORIENT_G_MIN, ORIENT_G_MAX): left out. Each axis bounded first, so the squares cannot overflow. */
	if (ax > ORIENT_G_MAX || ax < -ORIENT_G_MAX || ay > ORIENT_G_MAX || ay < -ORIENT_G_MAX || az > ORIENT_G_MAX ||
	    az < -ORIENT_G_MAX) {
		return d->current;
	}
	length2 = (int64_t)ax * ax + (int64_t)ay * ay + (int64_t)az * az;
	if (length2 < (int64_t)ORIENT_G_MIN * ORIENT_G_MIN || length2 > (int64_t)ORIENT_G_MAX * ORIENT_G_MAX) {
		return d->current;
	}

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

	/* mostly flat: gravity along z and neither horizontal pull strong -> face up or face down (the limit moved
	 * away from the orientation held by ORIENT_FLAT_HOLD, so it is not crossed back and forth) */
	flat_limit = ORIENT_FLAT_PULL;
	if (d->current == ORIENT_FACE_UP || d->current == ORIENT_FACE_DOWN) {
		flat_limit += ORIENT_FLAT_HOLD;
	} else if (d->current != ORIENT_UNKNOWN) {
		flat_limit -= ORIENT_FLAT_HOLD;
	}
	if (hx < flat_limit && hy < flat_limit) {
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
