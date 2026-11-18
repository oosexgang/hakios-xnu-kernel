/*
 * Host test of the motion-sensor pipeline (drivers/board/sensors_core.h): a raw accelerometer sample from a
 * chip, mounted turned on the board and in the chip's own counts, comes out as the right UIDeviceOrientation.
 * workbench/run.sh builds this against the kernel's own header.
 */
#include <stdio.h>

#include "board/sensors_core.h"

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

/* A chip mounted a quarter-turn from Darwin: Darwin x reads chip +y, Darwin y reads chip -x, Darwin z reads
 * chip +z; 16384 counts per g. The raw samples below are in the chip's frame and counts. */
static const struct sensor_mount TURNED = { { 1, 0, 2 }, { 1, -1, 1 } };
static const struct sensor_scale G16384 = { 1000, 16384 };

static int
settle(struct sensors_accel *s, int32_t rx, int32_t ry, int32_t rz, int n)
{
	int o = 0;
	for (int i = 0; i < n; i++) {
		o = sensors_accel_raw(s, rx, ry, rz);
	}
	return o;
}

static void
test_raw_chip_samples_become_the_right_orientation(void)
{
	struct sensors_accel s;
	s.mount = TURNED;
	s.scale = G16384;

	/* chip (16384,0,0) -> Darwin (0,-1000,0) milli-g -> portrait */
	sensors_accel_reset(&s);
	CHECK(settle(&s, 16384, 0, 0, 30) == ORIENT_PORTRAIT, "portrait: got %d", s.orient.current);

	/* chip (0,-16384,0) -> Darwin (-1000,0,0) -> landscape left */
	sensors_accel_reset(&s);
	CHECK(settle(&s, 0, -16384, 0, 30) == ORIENT_LANDSCAPE_LEFT, "landscape left: got %d", s.orient.current);

	/* chip (0,0,-16384) -> Darwin (0,0,-1000) -> face up */
	sensors_accel_reset(&s);
	CHECK(settle(&s, 0, 0, -16384, 30) == ORIENT_FACE_UP, "face up: got %d", s.orient.current);
}

static void
test_the_mounting_is_what_makes_it_right(void)
{
	/* the same raw sample, read as if the chip sat square (the identity mounting), reads a different
	 * orientation -- so the mounting is doing its job, not a coincidence of the axes */
	struct sensors_accel turned, square;
	turned.mount = TURNED;
	turned.scale = G16384;
	square.mount = (struct sensor_mount){ { 0, 1, 2 }, { 1, 1, 1 } };
	square.scale = G16384;
	sensors_accel_reset(&turned);
	sensors_accel_reset(&square);
	int t = settle(&turned, 16384, 0, 0, 30);
	int q = settle(&square, 16384, 0, 0, 30);
	CHECK(t == ORIENT_PORTRAIT, "turned reads portrait: %d", t);
	CHECK(q == ORIENT_LANDSCAPE_RIGHT, "square reads it as landscape (chip +x is Darwin +x): %d", q);
	CHECK(t != q, "the mounting changes the result");
}

int
main(void)
{
	test_raw_chip_samples_become_the_right_orientation();
	test_the_mounting_is_what_makes_it_right();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
