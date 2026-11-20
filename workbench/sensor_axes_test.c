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

static void
test_the_gyroscope_s_mounting(void)
{
	struct sensor_mount gyro;
	/* a chip square on the board reads +1 g up: the accelerometer's mount is every axis turned, the rates' none */
	struct sensor_mount square = { { 0, 1, 2 }, { -1, -1, -1 } };
	CHECK(sensor_gyro_mount(&square, &gyro) && gyro.axis[0] == 0 && gyro.axis[1] == 1 && gyro.axis[2] == 2 &&
	    gyro.sign[0] == 1 && gyro.sign[1] == 1 && gyro.sign[2] == 1, "square: %d%d%d %d %d %d", gyro.axis[0], gyro.axis[1],
	    gyro.axis[2], gyro.sign[0], gyro.sign[1], gyro.sign[2]);
	/* turned a quarter ("-y", "x", "-z"): the rates' Darwin x is the chip's +y, y the chip's -x, z the chip's +z */
	struct sensor_mount quarter = { { 1, 0, 2 }, { -1, 1, -1 } };
	CHECK(sensor_gyro_mount(&quarter, &gyro) && gyro.axis[0] == 1 && gyro.axis[1] == 0 && gyro.axis[2] == 2 &&
	    gyro.sign[0] == 1 && gyro.sign[1] == -1 && gyro.sign[2] == 1, "quarter: %d%d%d %d %d %d", gyro.axis[0], gyro.axis[1],
	    gyro.axis[2], gyro.sign[0], gyro.sign[1], gyro.sign[2]);
	struct sensor_sample out = sensor_to_darwin(&gyro, &(struct sensor_scale){ 1, 1 }, (struct sensor_sample){ { 100, 200, 300 } });
	CHECK(out.v[0] == 200 && out.v[1] == -100 && out.v[2] == 300, "a quarter's rates: %d %d %d", out.v[0], out.v[1], out.v[2]);
	/* swapped and one flip is a mirror (as an accelerometer's mount is); one turn less or more is not */
	struct sensor_mount swapped = { { 1, 0, 2 }, { 1, 1, 1 } };
	CHECK(sensor_gyro_mount(&swapped, &gyro) && gyro.sign[0] == -1 && gyro.sign[1] == -1 && gyro.sign[2] == -1, "a swap is a mirror");
	struct sensor_mount turned = { { 1, 0, 2 }, { 1, 1, -1 } };
	CHECK(!sensor_gyro_mount(&turned, &gyro), "a turn is not an accelerometer's mount");
	struct sensor_mount identity = { { 0, 1, 2 }, { 1, 1, 1 } };
	CHECK(!sensor_gyro_mount(&identity, &gyro), "the identity is not an accelerometer's mount (it would read +1 g up)");
	struct sensor_mount cycled = { { 1, 2, 0 }, { -1, -1, -1 } };
	CHECK(sensor_gyro_mount(&cycled, &gyro) && gyro.axis[0] == 1 && gyro.sign[0] == 1, "a cycle with every sign turned is a mirror");
	struct sensor_mount bad = { { 0, 0, 2 }, { -1, -1, -1 } };
	CHECK(!sensor_gyro_mount(&bad, &gyro), "not a mounting at all");
}

static void
test_the_gyroscope_s_scale(void)
{
	/* 70 mdps a count (an LSM6DSO at +-2000 dps): 1000 counts are 70 dps, 1.22173 rad/s */
	struct sensor_mount ident = { { 0, 1, 2 }, { 1, 1, 1 } };
	struct sensor_scale s = sensor_gyro_scale(70000);
	struct sensor_sample out = sensor_to_darwin(&ident, &s, (struct sensor_sample){ { 1000, -32768, 1 } });
	CHECK(out.v[0] == 1222, "70 dps in milli-rad/s: %d", out.v[0]);
	CHECK(out.v[1] == -40034, "-2293.76 dps: %d", out.v[1]);
	CHECK(out.v[2] == 1, "one count, 1.22 milli-rad/s: %d", out.v[2]);
	/* 180 degrees a second is pi radians: 18000000 counts at 10 udps are 3141.59 milli-rad/s */
	s = sensor_gyro_scale(10);
	out = sensor_to_darwin(&ident, &s, (struct sensor_sample){ { 18000000, 0, 0 } });
	CHECK(out.v[0] == 3142, "pi rad/s: %d", out.v[0]);
}

int
main(void)
{
	test_a_mounting_is_a_signed_permutation();
	test_the_identity_only_scales();
	test_a_mounting_turns_the_axes();
	test_scaling_rounds_to_nearest_both_signs();
	test_a_large_count_does_not_overflow();
	test_the_gyroscope_s_mounting();
	test_the_gyroscope_s_scale();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
