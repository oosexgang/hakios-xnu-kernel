/*
 * Host tests of the orientation detector (drivers/board/orientation_core.h): workbench/run.sh builds this
 * against the kernel's own header, so what is tested is what the kernel compiles. The detector is fed
 * accelerometer samples in milli-g and asked which UIDeviceOrientation they are.
 */
#include <stdio.h>
#include <stdlib.h>

#include "board/orientation_core.h"

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

/* feed one sample n times (so the low-pass settles) and return the orientation */
static int
settle(struct orient_detector *d, int32_t ax, int32_t ay, int32_t az, int n)
{
	int o = d->current;
	for (int i = 0; i < n; i++) {
		o = orient_update(d, ax, ay, az);
	}
	return o;
}

static void
test_each_orientation_from_rest(void)
{
	struct orient_detector d;
	/* a phone held still in each pose, gravity along one axis; a fresh detector each time */
	struct { int32_t x, y, z; int want; const char *name; } cases[] = {
		{    0,     0, -1000, ORIENT_FACE_UP,              "face up" },
		{    0,     0,  1000, ORIENT_FACE_DOWN,            "face down" },
		{    0, -1000,     0, ORIENT_PORTRAIT,             "portrait" },
		{    0,  1000,     0, ORIENT_PORTRAIT_UPSIDE_DOWN, "portrait upside down" },
		{-1000,     0,     0, ORIENT_LANDSCAPE_LEFT,       "landscape left" },
		{ 1000,     0,     0, ORIENT_LANDSCAPE_RIGHT,      "landscape right" },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		orient_reset(&d);
		int o = settle(&d, cases[i].x, cases[i].y, cases[i].z, 30);
		CHECK(o == cases[i].want, "%s: got %d, want %d", cases[i].name, o, cases[i].want);
	}
}

static void
test_the_first_sample_seeds_gravity(void)
{
	/* primed from the first sample, a phone already in portrait is named within a couple of samples, not
	 * after a slow climb from flat */
	struct orient_detector d;
	orient_reset(&d);
	int o = settle(&d, 0, -1000, 0, 2);
	CHECK(o == ORIENT_PORTRAIT, "seeded portrait settles at once: got %d", o);
}

static void
test_a_single_jolt_does_not_flip_it(void)
{
	/* held face up; one portrait-looking sample (a knock) must not change the orientation, the low-pass
	 * swallows it; a sustained portrait does change it */
	struct orient_detector d;
	orient_reset(&d);
	settle(&d, 0, 0, -1000, 30);
	CHECK(d.current == ORIENT_FACE_UP, "starts face up: %d", d.current);
	int o = orient_update(&d, 0, -1000, 0);     /* one jolt */
	CHECK(o == ORIENT_FACE_UP, "one jolt keeps face up: got %d", o);
	o = settle(&d, 0, -1000, 0, 30);            /* sustained */
	CHECK(o == ORIENT_PORTRAIT, "sustained turn becomes portrait: got %d", o);
}

static void
test_the_diagonal_holds_the_orientation(void)
{
	/* at 45 degrees between portrait and landscape neither axis dominates; whichever it was is kept, so a
	 * phone held on the diagonal does not flap between the two */
	struct orient_detector d;
	int o;

	orient_reset(&d);
	settle(&d, 0, -1000, 0, 30);                /* portrait */
	CHECK(d.current == ORIENT_PORTRAIT, "portrait first: %d", d.current);
	o = settle(&d, -700, -700, 0, 30);          /* exactly on the diagonal */
	CHECK(o == ORIENT_PORTRAIT, "diagonal keeps portrait: got %d", o);

	orient_reset(&d);
	settle(&d, -1000, 0, 0, 30);                /* landscape left */
	CHECK(d.current == ORIENT_LANDSCAPE_LEFT, "landscape first: %d", d.current);
	o = settle(&d, -700, -700, 0, 30);          /* the same diagonal, the other way in */
	CHECK(o == ORIENT_LANDSCAPE_LEFT, "diagonal keeps landscape: got %d", o);
}

static void
test_a_clear_turn_past_the_diagonal_switches(void)
{
	/* past the diagonal, the dominant axis names the new orientation */
	struct orient_detector d;
	orient_reset(&d);
	settle(&d, 0, -1000, 0, 30);                /* portrait */
	int o = settle(&d, -1000, -200, 0, 30);     /* clearly landscape now */
	CHECK(o == ORIENT_LANDSCAPE_LEFT, "a clear turn switches: got %d", o);
}

static void
test_tilting_up_from_portrait_becomes_face_up(void)
{
	/* laying the phone down from portrait: gravity moves from -y to -z, and once it is mostly along z it
	 * reads face up */
	struct orient_detector d;
	orient_reset(&d);
	settle(&d, 0, -1000, 0, 30);
	CHECK(d.current == ORIENT_PORTRAIT, "portrait first: %d", d.current);
	int o = settle(&d, 0, -200, -1000, 30);
	CHECK(o == ORIENT_FACE_UP, "laid down reads face up: got %d", o);
}

int
main(void)
{
	test_each_orientation_from_rest();
	test_the_first_sample_seeds_gravity();
	test_a_single_jolt_does_not_flip_it();
	test_the_diagonal_holds_the_orientation();
	test_a_clear_turn_past_the_diagonal_switches();
	test_tilting_up_from_portrait_becomes_face_up();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
