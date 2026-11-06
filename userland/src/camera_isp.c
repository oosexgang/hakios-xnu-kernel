/*
 * The cameras' frames made into pictures by the processor (see camera_isp.h): one pixel per 2x2
 * cell, the nearest cell for each pixel of the picture (previews), or every pixel by bilinear
 * demosaic (stills); per pixel, in this order: the black level off, lens shading (a grid of gains per
 * colour, when there is one), the white balance's and AE's gains, the colour matrix (identity until
 * one is set), sRGB's curve. Grey-world white balance; no noise reduction yet.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "camera_isp.h"

#define WHITE           959             /* 1023 less the black level of 64 */
#define LUT_WHITE       3836            /* WHITE in the gamma table's steps */

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	for (int i = 0; i < 3; i++) {
		isp->ccm[i][i] = 1;
	}
	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
	}
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

void
isp_measure(const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 };
	int r, b;

	memset(s, 0, sizeof(*s));
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += 4) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

		for (uint32_t cx = 0; cx < cells_w; cx += 4) {
			int v[4], g = 0;

			cell(top, f->stride, cx, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * Green's mean towards 15 % of white, unless that would put 1 % of the picture into white. The
 * exposure, longest first: the frame's lines at the shortest frame, then gain to 16x, then frames up
 * to max_frame_lines, then gain to the most; beyond that, digital gain (up to 4x).
 */
bool
isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c)
{
	double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
	double target = 0.15 * WHITE, mean = s->mean[1] * isp->digital > 0.5 ? s->mean[1] * isp->digital : 0.5;
	double bright = s->bright * isp->digital;
	uint32_t fast = f->frame_lines_min - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
	uint32_t lines, gain, frame_lines = 0;
	double digital = 1;

	if (isp->asked && (f->exposure_lines != isp->asked_lines || f->gain != isp->asked_gain)) {
		return false;                   /* (the last change not in the frames yet) */
	}
	/* (the measured levels are the sensor's: digital gain is applied to the picture only) */
	ratio = target / mean;
	if (bright * ratio > 0.9 * WHITE && bright > 0) {
		ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		return false;
	}
	want = now * pow(ratio, 0.7);
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain = f->gain_unity;
	} else if (want <= (double)fast * 16) {
		lines = fast;
		gain = (uint32_t)(want / fast * f->gain_unity);
	} else {
		lines = (uint32_t)(want / 16);
		lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
		frame_lines = lines + f->margin;
		double g = want / lines;

		if (g > (double)f->gain_max / f->gain_unity) {
			digital = g / ((double)f->gain_max / f->gain_unity);
			digital = digital > 4 ? 4 : digital;
			g = (double)f->gain_max / f->gain_unity;
		}
		gain = (uint32_t)(g * f->gain_unity);
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = gain;
	isp->asked_frame_lines = frame_lines;
	isp->asked = true;
	memset(c, 0, sizeof(*c));
	c->command = REDWOOD_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = gain;
	c->c = frame_lines;
	return true;
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	if (s->mean[0] < 2 || s->mean[2] < 2 || s->mean[1] < 2) {
		return;
	}
	float r = (float)(s->mean[1] / s->mean[0]), b = (float)(s->mean[1] / s->mean[2]);

	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->wb[0] = isp->wb[0] * 0.8f + r * 0.2f;
	isp->wb[2] = isp->wb[2] * 0.8f + b * 0.2f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions */
static void
crop(const struct redwood_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0, uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

void
isp_set_ccm(struct isp *isp, const float m[9])
{
	for (int i = 0; i < 9; i++) {
		isp->ccm[i / 3][i % 3] = m[i];
	}
}

/*
 * The colour matrix with the white balance's and AE's gains folded in, in the gamma table's steps
 * (per 10-bit step above black): out[c] = sum over j of m[c][j] * value[j] (each value lens-shaded).
 */
struct pipe {
	float           m[3][3];
	bool            identity;
};

static void
pipe_init(const struct isp *isp, struct pipe *p)
{
	p->identity = true;
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			p->m[c][j] = isp->ccm[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)WHITE);
			p->identity = p->identity && (c == j ? isp->ccm[c][j] == 1 : isp->ccm[c][j] == 0);
		}
	}
}

static inline uint8_t
curve(const struct isp *isp, float v)
{
	int i = (int)(v + 0.5f);

	return isp->gamma[i < 0 ? 0 : i > 4095 ? 4095 : i];
}

static inline void
pixel(const struct isp *isp, const struct pipe *p, float r, float g, float b, uint8_t *rgb)
{
	if (p->identity) {
		rgb[0] = curve(isp, r * p->m[0][0]);
		rgb[1] = curve(isp, g * p->m[1][1]);
		rgb[2] = curve(isp, b * p->m[2][2]);
		return;
	}
	rgb[0] = curve(isp, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
	rgb[1] = curve(isp, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
	rgb[2] = curve(isp, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}

/* the lens shading's gains at (u, v), 0 to 1 across the sensor */
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
	float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
	int x = (int)gx, y = (int)gy;

	x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fx = gx - x, fy = gy - y;

	for (int c = 0; c < 3; c++) {
		const float (*t)[LSC_W] = isp->lsc.gain[c];

		g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
	}
}

/* one line of the picture as 8-bit R, G, B */
static void
render_line(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb)
{
	uint32_t x0, y0, cw, ch, cells_w = f->width / 2, cells_h = f->height / 2;
	int r, b;
	struct pipe p;

	crop(f, width, height, &x0, &y0, &cw, &ch);
	cell_places(f->bayer, &r, &b);
	pipe_init(isp, &p);
	uint32_t cy = y0 + (uint32_t)((uint64_t)row * ch / height);
	const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
	uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;
	float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0, gains[3] = { 1, 1, 1 };

	for (uint32_t x = 0; x < width; x++, at += step) {
		uint32_t cx = (uint32_t)(at >> 16);
		int v[4], g = 0;

		cell(top, f->stride, cx, v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		if (isp->lsc.on) {
			lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
		}
		pixel(isp, &p, v[r] * gains[0], g * 0.5f * gains[1], v[b] * gains[2], rgb + 3 * x);
	}
}

/*
 * Two lines of 8-bit R, G, B as NV12 (BT.601's matrix, which CoreVideo's 420f and 420v and
 * tools/camraw.py take): each pixel's Y (y1 NULL: the second line has none, the picture's last of an
 * odd height), each 2x2's Cb and Cr from the sum of its four. The coefficients in 1/65536ths, video
 * range's scaled to its 219 steps of luma (16 to 235) and 224 of chroma (16 to 240); every value
 * rounded to the nearest step, so that the primaries come back within one step of 0 and 255 (red's and
 * blue's cannot be exact: no 8-bit Y, Cb, Cr is both).
 */
static void
nv12_pair(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
	const int32_t *k = video_range ? video : full;
	int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);

	for (int l = 0; l < 2; l++) {
		const uint8_t *p = l ? rgb1 : rgb0;
		uint8_t *out = l ? y1 : y0;

		for (uint32_t x = 0; out != NULL && x < width; x++, p += 3) {
			out[x] = (uint8_t)((k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + oy) >> 16);
		}
	}
	for (uint32_t x = 0; x < width; x += 2) {
		uint32_t x1 = x + 1 < width ? x + 1 : x;
		int32_t R = rgb0[3 * x] + rgb0[3 * x1] + rgb1[3 * x] + rgb1[3 * x1];
		int32_t G = rgb0[3 * x + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x + 1] + rgb1[3 * x1 + 1];
		int32_t B = rgb0[3 * x + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x + 2] + rgb1[3 * x1 + 2];
		/*
		 * (the sums are four pixels': 18 bits off, with 128 added and just under half a step to round;
		 * a tie goes down, so that chroma's -127.5, a full colour's, comes out 0 as its +127.5 does 255)
		 */
		int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
}

void
isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint8_t line[2][3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		bool second = row + 1 < height;

		render_line(isp, f, raw, width, height, row, line[0]);
		render_line(isp, f, raw, width, height, second ? row + 1 : row, line[1]);
		nv12_pair(line[0], line[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

void
isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		uint8_t *o = out + (size_t)row * stride;

		render_line(isp, f, raw, width, height, row, line);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = line[3 * x + 2];
			o[4 * x + 1] = line[3 * x + 1];
			o[4 * x + 2] = line[3 * x];
			o[4 * x + 3] = 255;
		}
	}
}

/* ---- every pixel: bilinear demosaic -------------------------------------------------------------------- */

/* the colour of each site of a 2x2 cell (0 R, 1 G, 2 B), position y * 2 + x */
static void
cfa(uint32_t bayer, int colour[4])
{
	int r, b;

	cell_places(bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		colour[i] = i == r ? 0 : i == b ? 2 : 1;
	}
}

/* line y of the frame as values above black (y clamped into the frame) */
static void
unpack_line(const struct redwood_camera_frame *f, const uint8_t *raw, int y, int *out)
{
	y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
	const uint8_t *line = raw + (size_t)y * f->stride;

	for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
		const uint8_t *g = line + (x >> 2) * 5;

		for (int i = 0; i < 4 && x + i < f->width; i++) {
			int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;

			out[x + i] = v < 0 ? 0 : v;
		}
	}
}

/* one line (y) at full size as 8-bit R, G, B, from the lines y - 1, y, y + 1 (l[0..2]) */
static void
full_line(const struct isp *isp, const struct redwood_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, uint8_t *rgb)
{
	int w = (int)f->width;
	float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };

	for (int x = 0; x < w; x++) {
		float sum[3] = { 0, 0, 0 };
		int n[3] = { 0, 0, 0 };

		/* each colour: its own value where the site has it, else the mean of its sites in the 3x3 around */
		for (int dy = -1; dy <= 1; dy++) {
			const int *line = l[dy + 1];
			int yy = (int)y + dy;

			yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
			for (int dx = -1; dx <= 1; dx++) {
				int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
				int c = colour[(yy & 1) * 2 + (xx & 1)];

				if (dx == 0 && dy == 0) {
					sum[c] = (float)line[xx] * 64;          /* (its own value wins over the neighbours') */
					n[c] = 64;
				} else if (n[c] < 64) {
					sum[c] += (float)line[xx];
					n[c]++;
				}
			}
		}
		if (isp->lsc.on) {
			lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
		}
		pixel(isp, p, sum[0] / (n[0] ? n[0] : 1) * gains[0], sum[1] / (n[1] ? n[1] : 1) * gains[1],
		    sum[2] / (n[2] ? n[2] : 1) * gains[2], rgb + 3 * x);
	}
}

void
isp_render_full_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t width = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[4];
	static __thread uint8_t *rgb[2];
	static __thread uint32_t have;

	if (width > have) {
		for (int i = 0; i < 4; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * width);
		}
		for (int i = 0; i < 2; i++) {
			free(rgb[i]);
			rgb[i] = malloc(3 * (size_t)width);
		}
		have = width;
	}
	cfa(f->bayer, colour);
	pipe_init(isp, &p);
	for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
		for (int i = 0; i < 4; i++) {
			unpack_line(f, raw, (int)row - 1 + i, lines[i]);
		}
		const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };

		bool second = row + 1 < f->height;

		full_line(isp, f, above, row, colour, &p, rgb[0]);
		full_line(isp, f, below, second ? row + 1 : row, colour, &p, rgb[1]);
		nv12_pair(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* ---- lens shading from a flat field --------------------------------------------------------------------- */

bool
isp_lsc_from_flat(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, float *min_ratio)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2, half_w = cells_w / (LSC_W - 1) / 2, half_h = cells_h / (LSC_H - 1) / 2;
	float level[3][LSC_H][LSC_W], top[3] = { 0, 0, 0 };
	uint32_t clipped = 0, seen = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int cx0 = (int)((uint64_t)gx * (cells_w - 1) / (LSC_W - 1)) - (int)half_w;
			int cy0 = (int)((uint64_t)gy * (cells_h - 1) / (LSC_H - 1)) - (int)half_h;
			double sum[3] = { 0, 0, 0 };
			uint32_t n = 0;

			for (int cy = cy0; cy <= cy0 + 2 * (int)half_h; cy += 2) {
				for (int cx = cx0; cx <= cx0 + 2 * (int)half_w; cx += 2) {
					int v[4];

					if (cx < 0 || cy < 0 || cx >= (int)cells_w || cy >= (int)cells_h) {
						continue;
					}
					cell(raw + (size_t)cy * 2 * f->stride, f->stride, (uint32_t)cx, v);
					sum[0] += v[r];
					sum[2] += v[b];
					for (int j = 0; j < 4; j++) {
						sum[1] += j != r && j != b ? v[j] / 2.0 : 0;
						clipped += v[j] > WHITE - 30;
					}
					seen += 4;
					n++;
				}
			}
			for (int c = 0; c < 3; c++) {
				level[c][gy][gx] = n ? (float)(sum[c] / n) : 0;
				top[c] = level[c][gy][gx] > top[c] ? level[c][gy][gx] : top[c];
			}
		}
	}
	/* bright enough to tell shading from noise, and not clipped anywhere that counts */
	if (top[1] < 100 || clipped * 1000 > seen) {
		return false;
	}
	float darkest = 1;

	for (int c = 0; c < 3; c++) {
		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				float l = level[c][gy][gx] > 1 ? level[c][gy][gx] : 1;

				isp->lsc.gain[c][gy][gx] = top[c] / l;
				if (c == 1 && l / top[c] < darkest) {
					darkest = l / top[c];
				}
			}
		}
	}
	isp->lsc.on = true;
	if (min_ratio != NULL) {
		*min_ratio = darkest;
	}
	return true;
}
