/*
 * Host tests of the thermal limiter's arithmetic (drivers/board/thermal_core.h).
 */
#include <stdio.h>

#include "board/thermal_core.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

int
main(void)
{
	const struct th_policy p = { 700, 620, 950 };
	int32_t r[4] = { TH_NONE, 455, 712, TH_NONE };
	uint32_t s;

	/* the sensor's word */
	CHECK(th_tsens_tenths(TSENS_VALID | 455) == 455, "%d", th_tsens_tenths(TSENS_VALID | 455));
	CHECK(th_tsens_tenths(455) == TH_NONE, "no valid bit");
	CHECK(th_tsens_tenths(TSENS_VALID | (0xfff & (uint32_t)-55)) == -55, "%d", th_tsens_tenths(TSENS_VALID | (0xfff & (uint32_t)-55)));
	CHECK(th_tsens_tenths(TSENS_VALID | (0xfff & (uint32_t)-2000)) == TH_NONE, "-200 C is no reading");
	CHECK(th_tsens_tenths(TSENS_VALID | 0x7ff) == TH_NONE, "204.7 C is no reading");
	CHECK(th_tsens_tenths(TSENS_VALID | 0xff000000u | 600) == 600, "high bits other than valid ignored");
	/* the hottest */
	CHECK(th_hottest(r, 4) == 712, "%d", th_hottest(r, 4));
	CHECK(th_hottest(r, 1) == TH_NONE, "none");
	/* the steps */
	CHECK(th_next_step(0, 5, 650, &p) == 0, "between clear and trip from cool: stays cool");
	CHECK(th_next_step(2, 5, 650, &p) == 2, "between: stays held");
	CHECK(th_next_step(2, 5, 700, &p) == 3, "at trip: one more");
	CHECK(th_next_step(5, 5, 800, &p) == 5, "never beyond most");
	CHECK(th_next_step(2, 5, 620, &p) == 1, "at clear: one less");
	CHECK(th_next_step(0, 5, 300, &p) == 0, "never below 0");
	CHECK(th_next_step(1, 5, 950, &p) == 5, "critical: all at once");
	CHECK(th_next_step(3, 5, TH_NONE, &p) == 3, "no reading: held where it is, not given back as if cool: %u", th_next_step(3, 5, TH_NONE, &p));
	CHECK(th_next_step(0, 5, TH_NONE, &p) == 0, "no reading from cool: not held back either");
	s = 5;
	for (int i = 0; i < 10; i++) {
		s = th_next_step(s, 5, TH_NONE, &p);
	}
	CHECK(s == 5, "held back all the way, then ten looks without a reading: still all the way: %u", s);
	CHECK(th_next_step(9, 5, 650, &p) == 5, "a step beyond most comes back within");
	CHECK(th_next_step(9, 5, TH_NONE, &p) == 5, "a step beyond most comes back within, a reading or none: %u", th_next_step(9, 5, TH_NONE, &p));
	/* a run: heating holds back a step a look, cooling gives back a step a look, hysteresis between */
	s = 0;
	for (int t = 600; t <= 760; t += 20) {
		s = th_next_step(s, 6, t, &p);
	}
	CHECK(s == 4, "after 700, 720, 740, 760: a step each: %u", s);
	for (int i = 0; i < 3; i++) {
		s = th_next_step(s, 6, 660, &p);
	}
	CHECK(s == 4, "held at 66 C: %u", s);
	for (int i = 0; i < 10; i++) {
		s = th_next_step(s, 6, 500, &p);
	}
	CHECK(s == 0, "cool: all given back: %u", s);
	/* rows */
	CHECK(th_row(20, 8, 0) == 20 && th_row(20, 8, 5) == 15 && th_row(20, 8, 12) == 8 && th_row(20, 8, 99) == 8, "rows");
	CHECK(th_row(5, 9, 3) == 5, "a floor above the top: the top, untouched");
	/* the panel */
	CHECK(th_backlight_cap(0, 6, 600) == 1000 && th_backlight_cap(6, 6, 600) == 600 && th_backlight_cap(3, 6, 600) == 800,
	    "%u %u %u", th_backlight_cap(0, 6, 600), th_backlight_cap(6, 6, 600), th_backlight_cap(3, 6, 600));
	CHECK(th_backlight_cap(3, 0, 600) == 1000, "nothing to hold back: full");
	printf("thermal_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
