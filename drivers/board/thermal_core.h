/*
 * The arithmetic of the thermal limiter (bsd/dev/board/thermal.c), apart from any hardware: what a
 * sensor's word says, how hot the phone is, how far to hold the processors back. Integers only, no
 * state; compiled by the host tests too (workbench/thermal_test.c).
 *
 * Temperatures are in tenths of a degree Celsius throughout.
 */
#ifndef _HAKIOS_THERMAL_CORE_H
#define _HAKIOS_THERMAL_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define TH_NONE                 (-2730)         /* no reading */
#define TH_PLAUSIBLE_LOW        (-400)
#define TH_PLAUSIBLE_HIGH       1500

/*
 * A Qualcomm TSENS (v2) sensor's status word: the last reading in its low 12 bits, signed, in tenths of a
 * degree, and bit 21 when there is one (Linux's tsens-v2.c). TH_NONE when there is none or it cannot be
 * true (a sensor not wired up reads -273 or nonsense).
 */
#define TSENS_VALID             (1u << 21)

static inline int32_t
th_tsens_tenths(uint32_t status)
{
	int32_t tenths;

	if (!(status & TSENS_VALID)) {
		return TH_NONE;
	}
	tenths = (int32_t)(status << 20) >> 20;
	return tenths < TH_PLAUSIBLE_LOW || tenths > TH_PLAUSIBLE_HIGH ? TH_NONE : tenths;
}

/* the hottest of `n` readings (TH_NONE if none of them is one) */
static inline int32_t
th_hottest(const int32_t *tenths, unsigned n)
{
	int32_t hottest = TH_NONE;

	for (unsigned i = 0; i < n; i++) {
		if (tenths[i] != TH_NONE && tenths[i] > hottest) {
			hottest = tenths[i];
		}
	}
	return hottest;
}

/*
 * How far the processors are held back: 0 not at all, each step one row of every group's frequency
 * table lower, up to `most`. Above `trip` a step more each look; at or below `clear` a step less; in
 * between it stays (so it does not go up and down around one temperature); at `critical` all the way at
 * once. No reading at all: it stays where it is, neither more nor less. It used to be one step less, as if it
 * were cool -- but a sensor goes silent for reasons that have nothing to do with the phone being cool (a
 * TSENS controller that stops answering, sensors that read nonsense as they get hot), and taken for cool
 * that gave every row back, a step a look, to a phone that was held back because it was hot, with nothing
 * left to hold it back but the hardware's own reset. Not knowing how hot it is is no reason to let it run
 * faster: what was held back stays held back until a sensor answers again and says it has cooled.
 */
struct th_policy {
	int32_t trip, clear, critical;
};

static inline uint32_t
th_next_step(uint32_t step, uint32_t most, int32_t hottest, const struct th_policy *p)
{
	if (hottest == TH_NONE) {
		return step > most ? most : step;
	}
	if (hottest >= p->critical) {
		return most;
	}
	if (hottest >= p->trip) {
		return step < most ? step + 1 : most;
	}
	if (hottest <= p->clear && step > 0) {
		return step - 1;
	}
	return step > most ? most : step;
}

/* a group's row at `step`: its top row, so many lower, never below its floor */
static inline uint32_t
th_row(uint32_t top, uint32_t floor, uint32_t step)
{
	if (floor > top) {
		floor = top;
	}
	return top - floor < step ? floor : top - step;
}

/*
 * The panel's light is heat too (an AMOLED at full white draws more than a core): the most the backlight
 * may show at `step` of `most`, 1000 when not held back, down to `lowest` when held back all the way.
 */
static inline uint32_t
th_backlight_cap(uint32_t step, uint32_t most, uint32_t lowest)
{
	if (step == 0 || most == 0) {
		return 1000;
	}
	if (step >= most) {
		return lowest;
	}
	return 1000 - (1000 - lowest) * step / most;
}

#endif /* _HAKIOS_THERMAL_CORE_H */
