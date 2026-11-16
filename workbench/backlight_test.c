/*
 * Host tests of the backlight's arithmetic (drivers/board/backlight_core.h): workbench/run.sh
 * builds this against the kernel's own header, so what is tested is what the kernel compiles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/backlight_core.h"

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
test_gamma(void)
{
	uint32_t last = 0;

	CHECK(bl_light_q16(0) == 0, "%u", bl_light_q16(0));
	CHECK(bl_light_q16(1000) == 65535, "%u", bl_light_q16(1000));
	CHECK(bl_light_q16(5000) == 65535, "beyond the top is the top");
	/* half the slider is about a fifth of the light (0.5 ^ 2.2 = 0.2176) */
	CHECK(bl_light_q16(500) > 14000 && bl_light_q16(500) < 14600, "%u", bl_light_q16(500));
	for (uint32_t level = 0; level <= 1000; level++) {
		uint32_t light = bl_light_q16(level);

		CHECK(light >= last, "not monotonic at %u: %u < %u", level, light, last);
		last = light;
	}
}

static void
test_raw(void)
{
	/* a panel of 1 to 2047 */
	CHECK(bl_level_to_raw(0, 1, 2047) == 1, "%u", bl_level_to_raw(0, 1, 2047));
	CHECK(bl_level_to_raw(1000, 1, 2047) == 2047, "%u", bl_level_to_raw(1000, 1, 2047));
	CHECK(bl_level_to_raw(500, 1, 2047) > 420 && bl_level_to_raw(500, 1, 2047) < 470, "%u", bl_level_to_raw(500, 1, 2047));
	/* a panel of 8-bit brightness */
	CHECK(bl_level_to_raw(1000, 0, 255) == 255, "%u", bl_level_to_raw(1000, 0, 255));
	CHECK(bl_level_to_raw(1, 0, 255) <= 1, "%u", bl_level_to_raw(1, 0, 255));
	/* nonsense limits: the least */
	CHECK(bl_level_to_raw(700, 50, 10) == 50, "%u", bl_level_to_raw(700, 50, 10));
	for (uint32_t level = 1, last = bl_level_to_raw(0, 8, 2047); level <= 1000; level++) {
		uint32_t raw = bl_level_to_raw(level, 8, 2047);

		CHECK(raw >= last && raw >= 8 && raw <= 2047, "8..2047 level %u -> %u", level, raw);
		last = raw;
	}
}

static void
test_dim(void)
{
	CHECK(bl_level_to_dim(1000, 16) == 256, "%u", bl_level_to_dim(1000, 16));
	CHECK(bl_level_to_dim(0, 16) == 16, "%u", bl_level_to_dim(0, 16));
	CHECK(bl_level_to_dim(0, 999) == 256, "a floor beyond 256 keeps all");
	CHECK(bl_dim_pixel(0xffffffffu, 256) == 0xffffffffu, "%08x", bl_dim_pixel(0xffffffffu, 256));
	CHECK(bl_dim_pixel(0xffffffffu, 0) == 0, "%08x", bl_dim_pixel(0xffffffffu, 0));
	CHECK(bl_dim_pixel(0xff808040u, 128) == 0x7f404020u, "%08x", bl_dim_pixel(0xff808040u, 128));
	/* every channel on its own, no carry between them */
	for (uint32_t dim = 0; dim <= 256; dim += 7) {
		for (uint32_t v = 0; v < 256; v += 5) {
			uint32_t p = v << 24 | (255 - v) << 16 | v << 8 | (v ^ 0x55);
			uint32_t q = bl_dim_pixel(p, dim);

			for (int shift = 0; shift < 32; shift += 8) {
				uint32_t in = p >> shift & 255, out = q >> shift & 255;

				CHECK(out == in * dim / 256, "dim %u channel %d: %u -> %u", dim, shift, in, out);
			}
		}
	}
}

static void
test_lux(void)
{
	uint32_t last = 0;

	CHECK(bl_log2_lux_q8(0) == 0, "%u", bl_log2_lux_q8(0));
	CHECK(bl_log2_lux_q8(1000) >= 250 && bl_log2_lux_q8(1000) <= 262, "log2(2) %u", bl_log2_lux_q8(1000));
	CHECK(bl_log2_lux_q8(1023000) >= 2550 && bl_log2_lux_q8(1023000) <= 2565, "log2(1024) %u", bl_log2_lux_q8(1023000));
	for (uint64_t mlx = 0; mlx < 4000000000ull; mlx = mlx * 11 / 10 + 1) {
		uint32_t q = bl_log2_lux_q8((uint32_t)mlx);

		CHECK(q >= last, "log not monotonic at %llu mlx", (unsigned long long)mlx);
		last = q;
	}
	/* the curve: dark, office, sun */
	CHECK(bl_auto_level(0) == 60, "%u", bl_auto_level(0));
	CHECK(bl_auto_level(300000) == 530, "%u", bl_auto_level(300000));
	CHECK(bl_auto_level(100000000) == 1000, "%u", bl_auto_level(100000000));
	last = 0;
	for (uint64_t mlx = 0; mlx < 4000000000ull; mlx = mlx * 11 / 10 + 1) {
		uint32_t level = bl_auto_level((uint32_t)mlx);

		CHECK(level >= last && level <= 1000, "curve at %llu mlx: %u after %u", (unsigned long long)mlx, level, last);
		last = level;
	}
	CHECK(bl_auto_target(300000, 200) == 730, "%u", bl_auto_target(300000, 200));
	CHECK(bl_auto_target(300000, -900) == 0, "%u", bl_auto_target(300000, -900));
	CHECK(bl_auto_target(100000000, 300) == 1000, "%u", bl_auto_target(100000000, 300));
	/* hysteresis: 10% no, 50% yes, either way */
	CHECK(!bl_lux_moved(110000, 100000), "10%% is a flicker");
	CHECK(bl_lux_moved(150000, 100000), "50%% up is a change");
	CHECK(bl_lux_moved(60000, 100000), "40%% down is a change");
}

static void
test_step(void)
{
	uint32_t now = 100 * 1024, target = 800 * 1024;
	int steps;

	for (steps = 0; now != target && steps < 1000; steps++) {
		now = bl_step(now, target);
	}
	CHECK(now == target, "brightening does not arrive");
	/* within 95% in about half a second at 20 steps a second */
	now = 100 * 1024;
	for (steps = 0; now < 765 * 1024; steps++) {
		now = bl_step(now, target);
	}
	CHECK(steps >= 5 && steps <= 30, "brightening took %d steps", steps);
	for (steps = 0; now > 135 * 1024; steps++) {
		now = bl_step(now, 100 * 1024);
	}
	CHECK(steps >= 40 && steps <= 200, "darkening took %d steps", steps);
	CHECK(bl_step(5, 5) == 5, "still");
}

static void
test_dcs(void)
{
	uint32_t w[4] = { 0 }, length = 0;
	unsigned n;

	/* 0x51, then 0x07 0xff for 2047 (MSB first) */
	n = bl_dcs_brightness_words(2047, 2, false, 0, w, &length);
	CHECK(n == 2 && length == 8, "n %u length %u", n, length);
	CHECK(w[0] == 0xc0390003u, "header %08x", w[0]);
	CHECK(w[1] == 0xffff0751u, "payload %08x", w[1]);
	n = bl_dcs_brightness_words(0x133, 2, true, 0, w, &length);
	CHECK(n == 2 && w[1] == 0xff013351u, "little endian payload %08x", w[1]);
	/* one byte: a short write, one word then the zero word the controller wants */
	memset(w, 0xaa, sizeof(w));
	n = bl_dcs_brightness_words(200, 1, false, 0, w, &length);
	CHECK(n == 2 && length == 4, "n %u length %u", n, length);
	CHECK(w[0] == 0x8015c851u, "short %08x", w[0]);
	CHECK(w[1] == 0, "padding word %08x", w[1]);
	n = bl_dcs_brightness_words(300, 1, false, 1, w, &length);
	CHECK(w[0] == 0x8055ff51u, "clamped, channel 1 %08x", w[0]);
	CHECK(bl_dcs_brightness_words(1, 3, false, 0, w, &length) == 0, "three bytes are not a thing");
}

int
main(void)
{
	test_gamma();
	test_raw();
	test_dim();
	test_lux();
	test_step();
	test_dcs();
	printf("backlight_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
