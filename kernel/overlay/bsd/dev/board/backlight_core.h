/*
 * The arithmetic of the backlight (bsd/dev/board/backlight.c), apart from any hardware: what a level of
 * the brightness slider is on a panel, what the light around the phone asks for, how the light follows,
 * and the command that tells a DSI panel its brightness. Integers only (the kernel has no floating
 * point), no state, nothing of the kernel's: the same file is compiled by the host tests
 * (workbench/backlight_test.c), so what is tested there is what runs here.
 *
 * Units, everywhere below:
 *   level      0 to 1000, the slider's position (Darwin's DisplayBrightness, 0.0 to 1.0, times 1000). The
 *              slider is perceptual: equal steps look like equal steps, which on a panel whose light
 *              grows in proportion to the number it is given means a power of about 2.2 between the two
 *   raw        the number the panel is given (a DSI panel's DBV, for example 1 to 2047)
 *   millilux   the ambient light, in thousandths of a lux (0 to 100,000 lux and some)
 *   dim        0 to 256: how much of every pixel is kept when the light is made in software
 */
#ifndef _HAKIOS_BACKLIGHT_CORE_H
#define _HAKIOS_BACKLIGHT_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define BL_LEVEL_MAX            1000
#define BL_Q16                  65536u

/* (i / 16) ^ 2.2 in 65536ths, i = 0 to 16 */
static const uint32_t bl_gamma_table[17] = {
	0, 147, 676, 1648, 3104, 5072, 7574, 10632, 14263, 18482, 23303, 28739, 34802, 41503, 48853, 56860, 65535,
};

/* A level as a fraction of the panel's whole light, in 65536ths (0 to 65535): level ^ 2.2. */
static inline uint32_t
bl_light_q16(uint32_t level)
{
	uint32_t at, i, part;

	if (level >= BL_LEVEL_MAX) {
		return bl_gamma_table[16];
	}
	at = level * 16 * 256 / BL_LEVEL_MAX;           /* where the level falls in the table, in 256ths of a step */
	i = at >> 8;
	part = at & 255;
	return bl_gamma_table[i] + (uint32_t)(((uint64_t)(bl_gamma_table[i + 1] - bl_gamma_table[i]) * part) >> 8);
}

/*
 * The number for the panel: its least at level 0 (Darwin's lowest brightness is dim, not dark), its most
 * at 1000, and the light in between growing as the eye expects it to.
 */
static inline uint32_t
bl_level_to_raw(uint32_t level, uint32_t raw_min, uint32_t raw_max)
{
	if (raw_max <= raw_min) {
		return raw_min;
	}
	return raw_min + (uint32_t)(((uint64_t)(raw_max - raw_min) * bl_light_q16(level) + 32767) / 65535);
}

/*
 * The light made in software, by darkening every pixel (a machine with no way to change its panel's
 * light: QEMU, or a panel not yet known): 256 keeps a pixel as it is. `floor` (0 to 256) is what is
 * kept at level 0, so that the lowest brightness is still readable.
 */
static inline uint32_t
bl_level_to_dim(uint32_t level, uint32_t floor)
{
	if (floor > 256) {
		floor = 256;
	}
	return floor + (uint32_t)(((uint64_t)(256 - floor) * bl_light_q16(level) + 32767) / 65535);
}

/* A pixel (four 8-bit channels in a word) darkened: each channel times dim / 256. */
static inline uint32_t
bl_dim_pixel(uint32_t pixel, uint32_t dim)
{
	uint32_t rb = ((pixel & 0x00ff00ffu) * dim >> 8) & 0x00ff00ffu;
	uint32_t ga = (((pixel >> 8) & 0x00ff00ffu) * dim) & 0xff00ff00u;

	return rb | ga;
}

/*
 * log2(1 + millilux / 1000), in 256ths (0 for darkness, about 4250 for 100,000 lux): the eye
 * goes by ratios of light, so the curve below is laid out on this.
 */
static inline uint32_t
bl_log2_lux_q8(uint32_t millilux)
{
	uint64_t x = (uint64_t)millilux + 1000;         /* 1 + lux, in thousandths */
	uint32_t whole = 0, frac;

	/* the whole part, from the highest bit of x / 1000 */
	while ((x >> 1) >= 1000) {
		x >>= 1;
		whole++;
	}
	/* now 1000 <= x < 2000: the rest, log2(x / 1000), by the curve log2(1 + t) ~ t (1.4 - 0.4 t) */
	{
		uint32_t t = (uint32_t)((x - 1000) * 256 / 1000);       /* 0 to 255 */

		frac = (t * (358 - (t * 102 >> 8))) >> 8;               /* 358 = 1.4 * 256, 102 = 0.4 * 256 */
		frac = frac > 255 ? 255 : frac;
	}
	return whole * 256 + frac;
}

/*
 * The light around the phone and the level it asks for. A curve of the usual shape (dark room: dim,
 * office: half, daylight: full), on log2(1 + lux); between its points straight lines.
 */
struct bl_point {
	uint32_t lux;
	uint32_t level;
};

static const struct bl_point bl_auto_curve[] = {
	{ 0, 60 }, { 2, 130 }, { 10, 220 }, { 30, 310 }, { 100, 410 }, { 300, 530 }, { 1000, 670 }, { 3000, 810 },
	{ 10000, 930 }, { 30000, 1000 },
};
#define BL_AUTO_POINTS          (sizeof(bl_auto_curve) / sizeof(bl_auto_curve[0]))

static inline uint32_t
bl_auto_level(uint32_t millilux)
{
	uint32_t at = bl_log2_lux_q8(millilux);

	for (unsigned i = 0; i + 1 < BL_AUTO_POINTS; i++) {
		uint32_t x0 = bl_log2_lux_q8(bl_auto_curve[i].lux * 1000), x1 = bl_log2_lux_q8(bl_auto_curve[i + 1].lux * 1000);
		uint32_t y0 = bl_auto_curve[i].level, y1 = bl_auto_curve[i + 1].level;

		if (at <= x1) {
			if (at <= x0 || x1 == x0) {
				return y0;
			}
			return y0 + (uint32_t)((uint64_t)(y1 - y0) * (at - x0) / (x1 - x0));
		}
	}
	return bl_auto_curve[BL_AUTO_POINTS - 1].level;
}

/* the curve moved by the user's own preference (the slider moved while the brightness is automatic) */
static inline uint32_t
bl_auto_target(uint32_t millilux, int32_t offset)
{
	int32_t level = (int32_t)bl_auto_level(millilux) + offset;

	return level < 0 ? 0 : level > BL_LEVEL_MAX ? BL_LEVEL_MAX : (uint32_t)level;
}

/*
 * Has the light changed enough to follow it? Not for every flicker: a change of more than about a
 * fifth (a quarter of a stop: 64 / 256) either way.
 */
#define BL_LUX_HYSTERESIS_Q8    64

static inline bool
bl_lux_moved(uint32_t millilux, uint32_t reference_millilux)
{
	uint32_t a = bl_log2_lux_q8(millilux), b = bl_log2_lux_q8(reference_millilux);

	return (a > b ? a - b : b - a) > BL_LUX_HYSTERESIS_Q8;
}

/*
 * One step of the light towards where it should be, `now` and `target` in 1024ths of a level: a
 * fraction of the way each step, quicker brightening than darkening (the eye minds a screen too
 * dark in a bright place more than the other way round), and at least an eighth of a level so that
 * the last of the way does not take long. At 20 steps a second, brightening is mostly done within
 * about a second, darkening within some four.
 */
#define BL_STEP_UP_DIV          4
#define BL_STEP_DOWN_DIV        16
#define BL_STEP_MIN             128

static inline uint32_t
bl_step(uint32_t now, uint32_t target)
{
	uint32_t move;

	if (now < target) {
		move = (target - now) / BL_STEP_UP_DIV;
		move = move < BL_STEP_MIN ? BL_STEP_MIN : move;
		return target - now < move ? target : now + move;
	}
	if (now > target) {
		move = (now - target) / BL_STEP_DOWN_DIV;
		move = move < BL_STEP_MIN ? BL_STEP_MIN : move;
		return now - target < move ? target : now - move;
	}
	return now;
}

/*
 * MIPI DCS set_display_brightness (0x51) as a Qualcomm DSI controller (2.2 and later) takes a command
 * in its FIFO: 32-bit words, of which the first is the packet's header in the controller's order
 * (two data bytes or the word count, then the data identifier, then the controller's own flags:
 * bit 7 the last command, bit 6 a long packet), then the payload padded with 0xff to a whole word,
 * and an even number of words (a zero word added: "the hardware requires it").
 *
 *   bytes = 1  short write with one parameter (DT 0x15): 0x51 v
 *   bytes = 2  long write (DT 0x39) of three bytes: 0x51 then the value most significant byte first,
 *              or least significant first with `little_endian` (the panels of both phones here are the
 *              first kind: "qcom,mdss-dsi-bl-inverted-dbv" in their device trees)
 *
 * Returns the number of words to put in the FIFO (`words`, at most 4), 0 if asked for something else;
 * `*length` is the command's length in bytes for DSI_DMA_CMD_LENGTH (the padding to a whole word in,
 * the extra word not).
 */
#define DCS_SET_DISPLAY_BRIGHTNESS      0x51
#define DSI_DT_DCS_SHORT_WRITE_1        0x15
#define DSI_DT_DCS_LONG_WRITE           0x39
#define DSI_CMD_LAST                    0x80
#define DSI_CMD_LONG                    0x40

static inline unsigned
bl_dcs_brightness_words(uint32_t raw, unsigned bytes, bool little_endian, unsigned channel, uint32_t words[4],
    uint32_t *length_out)
{
	uint8_t b[16];
	unsigned length, n;

	for (unsigned i = 0; i < sizeof(b); i++) {
		b[i] = 0xff;
	}
	channel &= 3;
	if (bytes == 1) {
		b[0] = DCS_SET_DISPLAY_BRIGHTNESS;
		b[1] = (uint8_t)(raw > 0xff ? 0xff : raw);
		b[2] = (uint8_t)(channel << 6 | DSI_DT_DCS_SHORT_WRITE_1);
		b[3] = DSI_CMD_LAST;
		length = 4;
	} else if (bytes == 2) {
		raw = raw > 0xffff ? 0xffff : raw;
		b[0] = 3;                       /* word count, low byte */
		b[1] = 0;                       /* ... high byte */
		b[2] = (uint8_t)(channel << 6 | DSI_DT_DCS_LONG_WRITE);
		b[3] = DSI_CMD_LAST | DSI_CMD_LONG;
		b[4] = DCS_SET_DISPLAY_BRIGHTNESS;
		b[5] = (uint8_t)(little_endian ? raw & 0xff : raw >> 8);
		b[6] = (uint8_t)(little_endian ? raw >> 8 : raw & 0xff);
		length = 7;                     /* padded with 0xff to 8 */
	} else {
		return 0;
	}
	n = (length + 3) / 4;
	*length_out = n * 4;
	for (unsigned i = 0; i < n; i++) {
		words[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
	}
	if (n & 1) {
		words[n++] = 0;
	}
	return n;
}

#endif /* _HAKIOS_BACKLIGHT_CORE_H */
