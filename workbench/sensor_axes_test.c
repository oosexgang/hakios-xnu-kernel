/*
 * Host tests of the sensor axis/unit arithmetic (drivers/board/sensor_axes_core.h): workbench/run.sh
 * builds this against the kernel's own header, so what is tested is what the kernel compiles.
 */
#include <stdio.h>

#include "board/sensor_axes_core.h"

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

static void
test_a_mounting_is_a_signed_permutation(void)
{
	struct sensor_mount ident = { { 0, 1, 2 }, { 1, 1, 1 } };
	CHECK(sensor_mount_ok(&ident), "the identity is a mounting");
	struct sensor_mount turned = { { 1, 0, 2 }, { -1, 1, -1 } };
	CHECK(sensor_mount_ok(&turned), "a turn and two flips is a mounting");
	struct sensor_mount repeat = { { 0, 0, 2 }, { 1, 1, 1 } };
	CHECK(!sensor_mount_ok(&repeat), "an axis used twice is not");
	struct sensor_mount wide = { { 0, 1, 3 }, { 1, 1, 1 } };
	CHECK(!sensor_mount_ok(&wide), "an axis past z is not");
	struct sensor_mount zero = { { 0, 1, 2 }, { 1, 0, 1 } };
	CHECK(!sensor_mount_ok(&zero), "a zero sign is not");
}

static void
test_the_identity_only_scales(void)
{
	struct sensor_mount ident = { { 0, 1, 2 }, { 1, 1, 1 } };
	struct sensor_scale g = { 1000, 16384 };   /* 16384 counts per g -> milli-g */
	struct sensor_sample out = sensor_to_darwin(&ident, &g, (struct sensor_sample){ { 16384, -16384, 8192 } });
	CHECK(out.v[0] == 1000, "x: %d", out.v[0]);
	CHECK(out.v[1] == -1000, "y: %d", out.v[1]);
	CHECK(out.v[2] == 500, "z (half a g): %d", out.v[2]);
}

static void
test_a_mounting_turns_the_axes(void)
{
	/* a chip whose +x is Darwin's -y, +y is Darwin's +x, +z is Darwin's +z (a 90-degree turn): Darwin x
	 * reads chip +y, Darwin y reads chip -x, Darwin z reads chip +z */
	struct sensor_mount m = { { 1, 0, 2 }, { 1, -1, 1 } };
	struct sensor_scale one = { 1, 1 };
	struct sensor_sample out = sensor_to_darwin(&m, &one, (struct sensor_sample){ { 100, 200, 300 } });
	CHECK(out.v[0] == 200, "x reads chip y: %d", out.v[0]);
	CHECK(out.v[1] == -100, "y reads chip -x: %d", out.v[1]);
	CHECK(out.v[2] == 300, "z reads chip z: %d", out.v[2]);
}

static void
test_scaling_rounds_to_nearest_both_signs(void)
{
	struct sensor_mount ident = { { 0, 1, 2 }, { 1, 1, 1 } };
	struct sensor_scale s = { 1, 3 };   /* divide by 3, to make halves */
	/* 5/3 = 1.67 -> 2; -5/3 -> -2; 1/3 -> 0; 4/3 -> 1 */
	struct sensor_sample out = sensor_to_darwin(&ident, &s, (struct sensor_sample){ { 5, -5, 1 } });
	CHECK(out.v[0] == 2, "5/3 rounds to 2: %d", out.v[0]);
	CHECK(out.v[1] == -2, "-5/3 rounds to -2: %d", out.v[1]);
	CHECK(out.v[2] == 0, "1/3 rounds to 0: %d", out.v[2]);
}

static void
test_a_large_count_does_not_overflow(void)
{
	/* a 32-bit count near its limit times a big numerator is held in 64 bits */
	struct sensor_mount ident = { { 0, 1, 2 }, { 1, 1, 1 } };
	struct sensor_scale s = { 1000000, 1000 };   /* times 1000 */
	struct sensor_sample out = sensor_to_darwin(&ident, &s, (struct sensor_sample){ { 2000000, 0, 0 } });
	CHECK(out.v[0] == 2000000000, "2e6 * 1000 / 1000 = 2e9: %d", out.v[0]);
}

int
main(void)
{
	test_a_mounting_is_a_signed_permutation();
	test_the_identity_only_scales();
	test_a_mounting_turns_the_axes();
	test_scaling_rounds_to_nearest_both_signs();
	test_a_large_count_does_not_overflow();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
