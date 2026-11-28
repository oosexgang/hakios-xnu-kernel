/*
 * Portrait, done by the processor: see camera_portrait.h.
 *
 * The views (grey) are matched as their fine detail (each pixel less the mean around it, scaled to the picture's own
 * contrast), so the two cameras' exposures and colours do not matter. The wide's view is scaled to the telephoto's
 * reach first (W, TELE_W x TELE_H, the middle 1/R of the wide's sensor), so a thing far away is the same size in both and
 * at the same place but for the offset of the telephoto's middle (dx0, dy0).
 *
 * Depth: a block of W at a time, looked for in the telephoto's view within SEARCH_NEAR..SEARCH_FAR pixels (sideways; SEARCH_Y up and down) of where it would be if it
 * were far away. How far from there it is found is its parallax (the nearer, the more). How sure: how much worse the next
 * best place not beside the best is. The blur of a block is by how much its parallax differs from the focus's, a little of
 * a pixel being as sharp. The blocks' blur is then filtered (a weighted median, the unsure ones filled from around them),
 * made finer along the edges of the wide's view (a joint bilateral upsampling: a block's share of a pixel less as their
 * brightness differs), and eased in over frames.
 */
#include "camera_portrait.h"

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * PSCALE: the views' size over the preview's. camera_portrait_photo.c builds this file again at 2 for the photograph (from
 * the two cameras' photographs): blocks and search as much bigger, so the same 40 x 30 blocks, the parallax found to half a
 * pixel of the preview's and kept in the preview's pixels, and the map twice as fine.
 */
#ifndef PSCALE
#define PSCALE          1
#endif
#define VW              (PORTRAIT_WIDE_W * PSCALE)
#define VH              (PORTRAIT_WIDE_H * PSCALE)
#define TW              (PORTRAIT_TELE_W * PSCALE)
#define TH              (PORTRAIT_TELE_H * PSCALE)
#define BLOCK           (8 * PSCALE)
#define BW              (TW / BLOCK)            /* 40 */
#define BH              (TH / BLOCK)            /* 30 */
/*
 * Along x (the cameras are side by side that way), from SEARCH_NEAR on the near side to SEARCH_FAR on the far side of
 * where the cameras' known line-up puts a thing: near is the negative side (boot 36: a thing a hand's length away at -22,
 * the room around 0), and nothing is much farther than the line-up's own distance. Index SEARCH_NEAR is 0.
 */
#define SEARCH_NEAR     (40 * PSCALE)
#define SEARCH_FAR      (8 * PSCALE)
#define SEARCH_N        (SEARCH_NEAR + SEARCH_FAR + 1)
#define SEARCH_Y        (PSCALE == 1 ? 3 : 4)   /* across: what the line-up leaves */
#define MW              (TW / 2)                /* the blur's map: 160 x 120 over W (320 x 240 for the photograph) */
#define MH              (TH / 2)
#define FIT_TRIES       3
#ifndef ENGINE
#define ENGINE          "portrait"      /* (what it says is said as) */
#endif

static portrait_say_fn say;
static dispatch_queue_t queue;
static pthread_mutex_t feed_lock = PTHREAD_MUTEX_INITIALIZER, map_lock = PTHREAD_MUTEX_INITIALIZER;

/* the views as fed, and the copies a depth job works on */
static uint8_t wide_in[VH][VW], tele_in[TH][TW], wide_job[VH][VW], tele_job[TH][TW];
static bool have_wide, have_tele, busy;
static volatile unsigned acc_runs;     /* depth runs into each block's averaged scores (below: acc) */
static unsigned tele_feeds;

/* how the cameras line up */
static bool fitted;
static int fit_tries;
static float fit_r, fit_score, best_r, best_score;
static int fit_o, fit_dx, fit_dy, best_o, best_dx, best_dy;

/* the blur, 0 (sharp) to 255 (as blurred as it gets) over W; and where the focus is in W (fractions) */
static float map_f[MH][MW];
static uint8_t map[MH][MW];
static bool have_map;
/* how much nearer than the focus, over W: the parallax over it (pixels of T, the near side positive); the photograph's zoom */
static float near_f[MH][MW];
static float still_zoom = 1;
static volatile float focus_u = 0.5f, focus_v = 0.5f;
static unsigned depth_runs;
static double depth_ms;

static bool dump_views;

void
portrait_init(portrait_say_fn fn, bool dump)
{
	say = fn;
	dump_views = dump;
	if (queue == NULL) {
		queue = dispatch_queue_create("hakios.portrait", DISPATCH_QUEUE_SERIAL);
	}
}

void
portrait_reset(void)
{
	pthread_mutex_lock(&feed_lock);
	have_wide = have_tele = false;
	pthread_mutex_unlock(&feed_lock);
	pthread_mutex_lock(&map_lock);
	have_map = false;
	pthread_mutex_unlock(&map_lock);
	acc_runs = 0;                           /* (the scores' average starts again) */
}

static double
ms_since(uint64_t t)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - t) * tb.numer / tb.denom / 1e6;
}

/* ---- the views --------------------------------------------------------------------------------------- */

/* W: the wide's view scaled to the telephoto's reach (the middle 1/r of the wide's sensor), TW x TH */
static void
wide_at(float r, uint8_t out[TH][TW])
{
	for (int y = 0; y < TH; y++) {
		float vy = (0.5f + (((y + 0.5f) / TH - 0.5f) / r) * PORTRAIT_WIDE_ZOOM) * VH - 0.5f;
		int y0 = (int)floorf(vy);
		float fy = vy - y0;
		int y1 = y0 + 1;

		y0 = y0 < 0 ? 0 : y0 >= VH ? VH - 1 : y0;
		y1 = y1 < 0 ? 0 : y1 >= VH ? VH - 1 : y1;
		for (int x = 0; x < TW; x++) {
			float vx = (0.5f + (((x + 0.5f) / TW - 0.5f) / r) * PORTRAIT_WIDE_ZOOM) * VW - 0.5f;
			int x0 = (int)floorf(vx);
			float fx = vx - x0;
			int x1 = x0 + 1;

			x0 = x0 < 0 ? 0 : x0 >= VW ? VW - 1 : x0;
			x1 = x1 < 0 ? 0 : x1 >= VW ? VW - 1 : x1;
			float top = wide_job[y0][x0] + (wide_job[y0][x1] - wide_job[y0][x0]) * fx;
			float bottom = wide_job[y1][x0] + (wide_job[y1][x1] - wide_job[y1][x0]) * fx;

			out[y][x] = (uint8_t)(top + (bottom - top) * fy + 0.5f);
		}
	}
}

/* the telephoto's view, turned: o's bit 0 left for right, bit 1 upside down */
static void
tele_as(int o, uint8_t out[TH][TW])
{
	for (int y = 0; y < TH; y++) {
		const uint8_t *row = tele_job[o & 2 ? TH - 1 - y : y];

		for (int x = 0; x < TW; x++) {
			out[y][x] = row[o & 1 ? TW - 1 - x : x];
		}
	}
}

/* w x h by f each way (box) */
static void
shrink(const uint8_t *in, int w, int h, int f, uint8_t *out)
{
	for (int y = 0; y < h / f; y++) {
		for (int x = 0; x < w / f; x++) {
			unsigned sum = 0;

			for (int j = 0; j < f; j++) {
				for (int i = 0; i < f; i++) {
					sum += in[(y * f + j) * w + x * f + i];
				}
			}
			out[y * (w / f) + x] = (uint8_t)(sum / (unsigned)(f * f));
		}
	}
}

/* smoothed (a [1 4 6 4 1] each way, twice: the telephoto's noise and softness, and the wide's to match), as floats */
static void
smooth(const uint8_t *in, int w, int h, float *out)
{
	float *tmp = malloc(sizeof(float) * (size_t)w * (size_t)h);

	for (int i = 0; i < w * h; i++) {
		out[i] = in[i];
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float s = 0;

				for (int i = -2; i <= 2; i++) {
					int xi = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i;

					s += out[y * w + xi] * (i == 0 ? 6 : abs(i) == 1 ? 4 : 1);
				}
				tmp[y * w + x] = s / 16;
			}
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float s = 0;

				for (int i = -2; i <= 2; i++) {
					int yi = y + i < 0 ? 0 : y + i >= h ? h - 1 : y + i;

					s += tmp[yi * w + x] * (i == 0 ? 6 : abs(i) == 1 ? 4 : 1);
				}
				out[y * w + x] = s / 16;
			}
		}
	}
	free(tmp);
}

/* how alike a's middle (margin m) and b moved by dx, dy are: their normalized cross-correlation, 1 the same */
static float
alike(const float *a, const float *b, int w, int h, int m, int dx, int dy)
{
	double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
	int n = (w - 2 * m) * (h - 2 * m);

	for (int y = m; y < h - m; y++) {
		const float *pa = a + y * w, *pb = b + (y + dy) * w + dx;

		for (int x = m; x < w - m; x++) {
			sa += pa[x], sb += pb[x], saa += pa[x] * pa[x], sbb += pb[x] * pb[x], sab += pa[x] * pb[x];
		}
	}
	double va = saa - sa * sa / n, vb = sbb - sb * sb / n;

	return va > 0 && vb > 0 ? (float)((sab - sa * sb / n) / sqrt(va * vb)) : 0;
}

/*
 * How the cameras line up: each way up of the telephoto's and each reach from 1.5 to 2.3, smoothed and at a quarter of the
 * size, every offset within 10 (40 of the full size); the best then at the full size, its reach within 0.05 and offset
 * within 5. The score: how alike they are there (1 the same). The best of FIT_TRIES pairs is kept.
 */
#define QW      (TW / 4)
#define QH      (TH / 4)
#define QOFF    10

static void
fit(void)
{
	static uint8_t w8[TH][TW], t8[TH][TW], wq[QH * QW], tq[QH * QW];
	static float ws[QH * QW], ts[4][QH * QW], wfull[TH * TW], tfull[TH * TW];
	uint64_t began = mach_absolute_time();
	float best = -2, r_best = 2;
	int o_best = 0, dx_best = 0, dy_best = 0;

	for (int o = 0; o < 4; o++) {
		tele_as(o, t8);
		shrink(&t8[0][0], TW, TH, 4, tq);
		smooth(tq, QW, QH, ts[o]);
	}
	for (int ri = 0; ri <= 16; ri++) {
		float r = 1.5f + 0.05f * ri;

		wide_at(r, w8);
		shrink(&w8[0][0], TW, TH, 4, wq);
		smooth(wq, QW, QH, ws);
		for (int o = 0; o < 4; o++) {
			for (int dy = -QOFF; dy <= QOFF; dy++) {
				for (int dx = -QOFF; dx <= QOFF; dx++) {
					float c = alike(ws, ts[o], QW, QH, QOFF + 1, dx, dy);

					if (c > best) {
						best = c, r_best = r, o_best = o, dx_best = dx * 4, dy_best = dy * 4;
					}
				}
			}
		}
	}

	/* finer */
	float fine = -2, r_fine = r_best;
	int dx_fine = dx_best, dy_fine = dy_best, m = 4 * QOFF + 8;

	tele_as(o_best, t8);
	smooth(&t8[0][0], TW, TH, tfull);
	for (int ri = -5; ri <= 5; ri++) {
		float r = r_best + 0.01f * ri;

		wide_at(r, w8);
		smooth(&w8[0][0], TW, TH, wfull);
		for (int dy = dy_best - 5; dy <= dy_best + 5; dy++) {
			for (int dx = dx_best - 5; dx <= dx_best + 5; dx++) {
				float c = alike(wfull, tfull, TW, TH, m, dx, dy);

				if (c > fine) {
					fine = c, r_fine = r, dx_fine = dx, dy_fine = dy;
				}
			}
		}
	}
	float score = fine;

	fit_tries++;
	say("portrait: the cameras line up (try %d): reach %.2f, the telephoto's view %s, offset %d,%d; %.3f alike (%.0f ms)",
	    fit_tries, r_fine, o_best == 0 ? "as it is" : o_best == 1 ? "left for right" : o_best == 2 ? "upside down" :
	    "turned half round", dx_fine, dy_fine, score, ms_since(began));
	if (fit_tries == 1 || score > best_score) {
		best_score = score, best_r = r_fine, best_o = o_best, best_dx = dx_fine, best_dy = dy_fine;
	}
	if (fit_tries >= FIT_TRIES && dump_views) {
		/* the views as they were, for working on the depth elsewhere: "portrait-dump W|T row piece hex" */
		static const char hex[] = "0123456789abcdef";
		char line[2 * 160 + 1];

		for (int t = 0; t < 2; t++) {
			int w = t ? TW : VW, h = t ? TH : VH;
			const uint8_t *v = t ? &tele_job[0][0] : &wide_job[0][0];

			for (int y = 0; y < h; y++) {
				for (int x0 = 0; x0 < w; x0 += 160) {
					int n = w - x0 < 160 ? w - x0 : 160;

					for (int i = 0; i < n; i++) {
						line[2 * i] = hex[v[y * w + x0 + i] >> 4];
						line[2 * i + 1] = hex[v[y * w + x0 + i] & 15];
					}
					line[2 * n] = 0;
					say("portrait-dump %c %d %d %s", t ? 'T' : 'W', y, x0, line);
				}
			}
		}
	}
	if (fit_tries >= FIT_TRIES) {
		/*
		 * The cameras do not move: on a far scene (boot 39, a room 2 m and more away) the fit is 1.96, as it is, -6,15
		 * at 0.976, and the depth from it follows the scene (the near things to one side, the far to the other). A
		 * near thing filling the view pulls the fit away (boots 34-38: 1.50, -16,15 even at 0.946): a fit that is weak
		 * or far from the known one is the scene's parallax, not the line-up, and the known one is kept instead.
		 * (Boots 20-33's 1.80, -28,24 was on the cameras as they were then set up.)
		 */
		if (best_score < 0.95f || fabsf(best_r - 1.96f) > 0.06f || best_o != 0 || abs(best_dx + 6) > 8 ||
		    abs(best_dy - 15) > 8) {
			say("portrait: the fit (reach %.2f, way %d, offset %d,%d, %.3f alike) is not the cameras' known line-up: "
			    "that is kept", best_r, best_o, best_dx, best_dy, best_score);
			best_r = 1.96f, best_o = 0, best_dx = -6, best_dy = 15;
		}
		fit_r = best_r, fit_o = best_o, fit_dx = best_dx, fit_dy = best_dy, fit_score = best_score;
		fitted = true;
		say("portrait: kept: reach %.2f, way %d, offset %d,%d (%.3f alike)%s", fit_r, fit_o, fit_dx, fit_dy,
		    fit_score, fit_score < 0.8f ? " -- WEAK: little to go by in the picture" : "");
	}
}

/* ---- the depth --------------------------------------------------------------------------------------- */

static uint8_t w8[TH][TW], t8[TH][TW], guide[MH][MW];
static float wsm[TH * TW], tsm[TH * TW];
static float rx[BH][BW], ry[BH][BW], conf[BH][BW];
/*
 * Each block's scores, averaged over the frames (each new one a quarter of it: the last 4 or 5): the telephoto's noise
 * differs from frame to frame and the scene, the phone held still, does not. acc_runs 0: none yet (the next taken as it is).
 */
#define ACC_NEW 0.25f
static float acc[BH][BW][2 * SEARCH_Y + 1][SEARCH_N];

/*
 * A block of 16x16 of W around each 8x8 (the blocks overlap), looked for in the telephoto's view within the search, how alike
 * each place is (normalized cross-correlation, on both views smoothed). Sure: the best is alike (0.7 and up: nothing; 0.9:
 * all of it), stands out from the next best place not beside it (by 0.1: all of it), and the block is not flat.
 */
#define MATCH   (16 * PSCALE)

static void
match_row(void *context, size_t by)
{
	(void)context;
	for (int bx = 0; bx < BW; bx++) {
		enum { n = SEARCH_N, ny = 2 * SEARCH_Y + 1 };
		float score[ny][n], a[MATCH * MATCH], best = -2;
		int x0 = bx * BLOCK + BLOCK / 2 - MATCH / 2, y0 = (int)by * BLOCK + BLOCK / 2 - MATCH / 2, bi = SEARCH_NEAR, bj = SEARCH_Y;
		double sa = 0, saa = 0;

		x0 = x0 < 0 ? 0 : x0 > TW - MATCH ? TW - MATCH : x0;
		y0 = y0 < 0 ? 0 : y0 > TH - MATCH ? TH - MATCH : y0;
		for (int y = 0; y < MATCH; y++) {
			for (int x = 0; x < MATCH; x++) {
				float v = wsm[(y0 + y) * TW + x0 + x];

				a[y * MATCH + x] = v, sa += v, saa += v * v;
			}
		}
		float mean = (float)(sa / (MATCH * MATCH)), var = (float)(saa / (MATCH * MATCH)) - mean * mean;

		rx[by][bx] = ry[by][bx] = conf[by][bx] = 0;
		if (var < 1.5f * 1.5f) {
			/* (flat: says nothing; its average starts again when it is not) */
			for (int j = 0; j < 2 * SEARCH_Y + 1; j++) {
				for (int i = 0; i < SEARCH_N; i++) {
					acc[by][bx][j][i] = -2;
				}
			}
			continue;
		}
		for (int i = 0; i < MATCH * MATCH; i++) {
			a[i] -= mean;
		}
		for (int j = 0; j < ny; j++) {
			for (int i = 0; i < n; i++) {
				int tx = x0 + fit_dx * PSCALE + i - SEARCH_NEAR, ty = y0 + fit_dy * PSCALE + j - SEARCH_Y;
				float sb = 0, sbb = 0, sab = 0;

				score[j][i] = -2;
				if (tx < 0 || ty < 0 || tx + MATCH > TW || ty + MATCH > TH) {
					continue;
				}
				for (int y = 0; y < MATCH; y++) {
					const float *pa = a + y * MATCH, *pb = tsm + (ty + y) * TW + tx;

					for (int x = 0; x < MATCH; x++) {
						sb += pb[x], sbb += pb[x] * pb[x], sab += pa[x] * pb[x];
					}
				}
				float vb = sbb - sb * sb / (MATCH * MATCH);

				score[j][i] = vb > 0 ? sab / sqrtf(var * MATCH * MATCH * vb) : 0;
			}
		}
		/* into the average, and the best of that */
		float (*avg)[n] = acc[by][bx];

		for (int j = 0; j < ny; j++) {
			for (int i = 0; i < n; i++) {
				avg[j][i] = acc_runs == 0 || score[j][i] == -2 || avg[j][i] == -2 ? score[j][i] :
				    avg[j][i] * (1 - ACC_NEW) + score[j][i] * ACC_NEW;
				if (avg[j][i] > best) {
					best = avg[j][i], bi = i, bj = j;
				}
			}
		}
		float second = -2;

		for (int j = 0; j < ny; j++) {
			for (int i = 0; i < n; i++) {
				if (abs(i - bi) >= 2 * PSCALE && avg[j][i] > second) {
					second = avg[j][i];
				}
			}
		}
		float stand = (best - second) / 0.1f, good = (best - 0.7f) / 0.2f;

		stand = stand < 0 ? 0 : stand > 1 ? 1 : stand;
		good = good < 0 ? 0 : good > 1 ? 1 : good;
		rx[by][bx] = (float)(bi - SEARCH_NEAR) / PSCALE;      /* (in the preview's pixels) */
		ry[by][bx] = (float)(bj - SEARCH_Y) / PSCALE;
		/* (best at the search's edge: the true place may be beyond it -- not to be trusted) */
		conf[by][bx] = bi == 0 || bi == n - 1 ? 0 : stand * good;
	}
}

static int
cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

struct pair {
	float v, w;
};

static int
cmp_pair(const void *a, const void *b)
{
	return cmp_float(&((const struct pair *)a)->v, &((const struct pair *)b)->v);
}

static float
weighted_median(struct pair *p, int n)
{
	float total = 0, run = 0;

	qsort(p, (size_t)n, sizeof(*p), cmp_pair);
	for (int i = 0; i < n; i++) {
		total += p[i].w;
	}
	for (int i = 0; i < n; i++) {
		run += p[i].w;
		if (run >= total / 2) {
			return p[i].v;
		}
	}
	return n > 0 ? p[n - 1].v : 0;
}

/*
 * Each block's parallax made to agree with its neighbours' (semi-global: the scores carried along 8 ways across the blocks,
 * a small cost for a step of 1 and a bigger one for a jump), so flat and unsure blocks take their surroundings' and one
 * block's chance match does not stand alone. sd: the parallax of each block (preview pixels, a fraction from the scores on
 * either side of the best).
 */
#define SGM_P1  0.05f
#define SGM_P2  0.4f

static float sgm_cost[BH][BW][SEARCH_N], sgm_path[BH][BW][SEARCH_N], sgm_sum[BH][BW][SEARCH_N];
static float sd[BH][BW];

static void
sgm_way(int dx, int dy)
{
	for (int yy = 0; yy < BH; yy++) {
		for (int xx = 0; xx < BW; xx++) {
			int y = dy < 0 ? BH - 1 - yy : yy, x = dx < 0 ? BW - 1 - xx : xx, px = x - dx, py = y - dy;
			float *l = sgm_path[y][x], *c = sgm_cost[y][x];

			if (px < 0 || py < 0 || px >= BW || py >= BH) {
				memcpy(l, c, sizeof(sgm_path[0][0]));
			} else {
				const float *lp = sgm_path[py][px];
				float m = lp[0];

				for (int d = 1; d < SEARCH_N; d++) {
					m = lp[d] < m ? lp[d] : m;
				}
				for (int d = 0; d < SEARCH_N; d++) {
					float v = lp[d];

					if (d > 0 && lp[d - 1] + SGM_P1 < v) {
						v = lp[d - 1] + SGM_P1;
					}
					if (d < SEARCH_N - 1 && lp[d + 1] + SGM_P1 < v) {
						v = lp[d + 1] + SGM_P1;
					}
					v = m + SGM_P2 < v ? m + SGM_P2 : v;
					l[d] = c[d] + v - m;
				}
			}
			for (int d = 0; d < SEARCH_N; d++) {
				sgm_sum[y][x][d] += l[d];
			}
		}
	}
}

static void
aggregate(void)
{
	static const int ways[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };

	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			for (int d = 0; d < SEARCH_N; d++) {
				float b = -2;

				for (int j = 0; j < 2 * SEARCH_Y + 1; j++) {
					b = acc[by][bx][j][d] > b ? acc[by][bx][j][d] : b;
				}
				sgm_cost[by][bx][d] = b == -2 ? 0 : (1 - b) * 0.5f;    /* (flat or outside: no say) */
			}
		}
	}
	memset(sgm_sum, 0, sizeof(sgm_sum));
	for (int i = 0; i < 8; i++) {
		sgm_way(ways[i][0], ways[i][1]);
	}
	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			const float *s = sgm_sum[by][bx];
			int bd = 0;
			float f = 0;

			for (int d = 1; d < SEARCH_N; d++) {
				bd = s[d] < s[bd] ? d : bd;
			}
			if (bd > 0 && bd < SEARCH_N - 1) {
				float den = s[bd - 1] - 2 * s[bd] + s[bd + 1];

				f = den > 1e-6f ? 0.5f * (s[bd - 1] - s[bd + 1]) / den : 0;
			}
			sd[by][bx] = (bd + f - SEARCH_NEAR) / PSCALE;
		}
	}
}

static void
depth(void)
{
	static float a[BH][BW], a2[BH][BW], w[BH][BW], w2[BH][BW], mean[BH][BW], p[BH][BW], p2[BH][BW];
	static float fresh[MH][MW], fresh_p[MH][MW];
	static struct pair side[BH * BW];
	static float focus_x, focus_y;
	uint64_t began = mach_absolute_time();

	wide_at(fit_r, w8);
	tele_as(fit_o, t8);
	smooth(&w8[0][0], TW, TH, wsm);
	smooth(&t8[0][0], TW, TH, tsm);
	dispatch_apply_f(BH, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, match_row);
	acc_runs++;
	aggregate();

	/*
	 * The focus's parallax: the weighted median of the sure blocks around it, from within 2 of it out to within 12 until
	 * they are sure enough together (3 blocks' worth); else as it was.
	 */
	static struct pair px[25 * 25], py[25 * 25];
	int fbx = (int)(focus_u * BW), fby = (int)(focus_v * BH);

	for (int r = 2; r <= 12; r += 2) {
		int n = 0;
		float sure = 0;

		for (int by = fby - r; by <= fby + r; by++) {
			for (int bx = fbx - r; bx <= fbx + r; bx++) {
				if (bx >= 0 && by >= 0 && bx < BW && by < BH && conf[by][bx] > 0.2f) {
					px[n] = (struct pair){ sd[by][bx], conf[by][bx] };
					py[n] = (struct pair){ ry[by][bx], conf[by][bx] };
					sure += conf[by][bx];
					n++;
				}
			}
		}
		if (sure >= 3) {
			focus_x = weighted_median(px, n);
			focus_y = weighted_median(py, n);
			break;
		}
	}

	/* each block's blur: none within 1.5 pixels of the focus's parallax (the matching's noise), all at 4.5 */
	int confident = 0, sides = 0;

	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			float d = fabsf(sd[by][bx] - focus_x), v = (d - 1.5f) / 3.0f;   /* (sideways only: up and down is the line-up's noise) */

			a[by][bx] = v < 0 ? 0 : v > 1 ? 1 : v;
			w[by][bx] = 1;                  /* (every block has its parallax now: see aggregate) */
			confident += conf[by][bx] > 0.2f;
			if (conf[by][bx] > 0.2f) {
				side[sides++] = (struct pair){ rx[by][bx] - focus_x, conf[by][bx] };
			}
		}
	}
	/* how much nearer than the line-up's distance (the near side is the negative one: see SEARCH_NEAR) */
	(void)sides;
	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			p[by][bx] = -sd[by][bx];
		}
	}
	/* a weighted median of the 3x3 around each block; the unsure with none sure around them filled from around them */
	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			struct pair q[9], qp[9];
			float total = 0;
			int k = 0;

			for (int j = -1; j <= 1; j++) {
				for (int i = -1; i <= 1; i++) {
					int x = bx + i, y = by + j;

					if (x >= 0 && y >= 0 && x < BW && y < BH && w[y][x] > 0) {
						qp[k] = (struct pair){ p[y][x], w[y][x] };
						q[k++] = (struct pair){ a[y][x], w[y][x] };
						total += w[y][x];
					}
				}
			}
			a2[by][bx] = k > 0 ? weighted_median(q, k) : 0;
			p2[by][bx] = k > 0 ? weighted_median(qp, k) : 0;
			w2[by][bx] = total > 0.2f ? 1 : 0;
		}
	}
	for (int pass = 0, empty = 1; pass < 40 && empty; pass++) {
		empty = 0;
		memcpy(w, w2, sizeof(w));
		memcpy(a, a2, sizeof(a));
		memcpy(p, p2, sizeof(p));
		for (int by = 0; by < BH; by++) {
			for (int bx = 0; bx < BW; bx++) {
				float s = 0, sp = 0;
				int k = 0;

				if (w[by][bx] > 0) {
					continue;
				}
				for (int j = -1; j <= 1; j++) {
					for (int i = -1; i <= 1; i++) {
						int x = bx + i, y = by + j;

						if (x >= 0 && y >= 0 && x < BW && y < BH && w[y][x] > 0) {
							s += a[y][x], sp += p[y][x], k++;
						}
					}
				}
				if (k > 0) {
					a2[by][bx] = 0.8f * s / k, w2[by][bx] = 1;     /* (fading to sharp away from what is known) */
					p2[by][bx] = 0.8f * sp / k;
				} else {
					empty = 1;
				}
			}
		}
	}

	/* finer, along the wide's edges: the guide (W at half its size) and each block's mean brightness in it */
	shrink(&w8[0][0], TW, TH, 2, &guide[0][0]);
	for (int by = 0; by < BH; by++) {
		for (int bx = 0; bx < BW; bx++) {
			float s = 0;

			for (int j = 0; j < BLOCK / 2; j++) {
				for (int i = 0; i < BLOCK / 2; i++) {
					s += guide[by * (BLOCK / 2) + j][bx * (BLOCK / 2) + i];
				}
			}
			mean[by][bx] = s / ((BLOCK / 2) * (BLOCK / 2));
		}
	}
	for (int y = 0; y < MH; y++) {
		float cy = (y + 0.5f) / (BLOCK / 2) - 0.5f;
		int y0 = (int)floorf(cy);

		for (int x = 0; x < MW; x++) {
			float cx = (x + 0.5f) / (BLOCK / 2) - 0.5f, s = 0, sp = 0, sw = 0, g = guide[y][x];
			int x0 = (int)floorf(cx);

			for (int j = y0 - 1; j <= y0 + 2; j++) {
				for (int i = x0 - 1; i <= x0 + 2; i++) {
					int bx = i < 0 ? 0 : i >= BW ? BW - 1 : i, by = j < 0 ? 0 : j >= BH ? BH - 1 : j;
					float ddx = cx - i, ddy = cy - j, dl = g - mean[by][bx];
					float k = expf(-(ddx * ddx + ddy * ddy) / 2.0f - dl * dl / (2 * 14.0f * 14.0f));

					s += k * a2[by][bx], sp += k * p2[by][bx], sw += k;
				}
			}
			fresh[y][x] = sw > 0 ? s / sw : 1;
			fresh_p[y][x] = sw > 0 ? sp / sw : 0;
		}
	}

	/*
	 * Too little to go by (a flat or dark scene: under 10 % of the blocks sure, the rest only filled in from them): no
	 * blur rather than all of it.
	 */
	bool little = confident < BW * BH / 10;

	pthread_mutex_lock(&map_lock);
	for (int y = 0; y < MH; y++) {
		for (int x = 0; x < MW; x++) {
			float v = little ? 0 : fresh[y][x];

			float n = little ? 0 : fresh_p[y][x];

			map_f[y][x] = have_map ? map_f[y][x] * 0.5f + v * 0.5f : v;
			map[y][x] = (uint8_t)(map_f[y][x] * 255 + 0.5f);
			near_f[y][x] = have_map ? near_f[y][x] * 0.5f + n * 0.5f : n;
		}
	}
	have_map = true;
	pthread_mutex_unlock(&map_lock);

	depth_ms += ms_since(began);
	if (++depth_runs == 1 || depth_runs % 100 == 0) {
		float total = 0;

		for (int y = 0; y < MH; y++) {
			for (int x = 0; x < MW; x++) {
				total += map_f[y][x];
			}
		}
		say(ENGINE ": depth %u: %.1f ms each; %d of %d blocks sure; the focus at %.2f,%.2f, its parallax %.1f,%.1f; "
		    "%.0f %% blurred%s", depth_runs, depth_ms / depth_runs, confident, BW * BH, focus_u, focus_v, focus_x, focus_y,
		    100 * total / (MW * MH), little ? " (too little to go by: sharp)" : "");
	}
}

static void
job(void *context)
{
	(void)context;
	if (!fitted) {
		fit();
	} else {
		depth();
	}
	pthread_mutex_lock(&feed_lock);
	busy = false;
	pthread_mutex_unlock(&feed_lock);
}

void
portrait_feed(bool tele, const uint8_t *grey, size_t stride)
{
	int w = tele ? TW : VW, h = tele ? TH : VH;
	uint8_t *to = tele ? &tele_in[0][0] : &wide_in[0][0];

	pthread_mutex_lock(&feed_lock);
	for (int y = 0; y < h; y++) {
		memcpy(to + y * w, grey + y * stride, (size_t)w);
	}
	*(tele ? &have_tele : &have_wide) = true;
	if (tele) {
		tele_feeds++;
	}
	/*
	 * A job at each telephoto's view while none is going (fitting: from the 10th, every 10th, the exposures settled), on
	 * the queue: the camera's thread goes on.
	 */
	if (tele && have_wide && !busy && queue != NULL && (fitted || tele_feeds % 10 == 0)) {
		busy = true;
		memcpy(wide_job, wide_in, sizeof(wide_job));
		memcpy(tele_job, tele_in, sizeof(tele_job));
		dispatch_async_f(queue, NULL, job);
	}
	pthread_mutex_unlock(&feed_lock);
}

/* ---- the blur ---------------------------------------------------------------------------------------- */

#define SMALL_MAX       (4096 / 4)

struct blend {
	uint8_t *y, *uv;
	size_t y_stride, uv_stride;
	uint32_t width, height, sw, sh;
	const uint8_t *small_y, *small_uv;
	/* per column and per row: the small picture's (index, the next, the fraction in 256ths) and the map's (-1: outside) */
	int32_t *sx0, *sx1, *mx0, *mx1;
	uint8_t *sxf, *mxf;
	int32_t *sy0, *sy1, *my0, *my1;
	uint8_t *syf, *myf;
	const uint8_t (*map)[MW];
	bool show;
};

/* the blur at pixel x of row y (0..256) */
static inline unsigned
blur_at(const struct blend *b, uint32_t x, uint32_t y)
{
	if (b->mx0[x] < 0 || b->my0[y] < 0) {
		return 256;
	}
	const uint8_t *r0 = b->map[b->my0[y]], *r1 = b->map[b->my1[y]];
	unsigned fx = b->mxf[x], fy = b->myf[y];
	unsigned top = r0[b->mx0[x]] * (256 - fx) + r0[b->mx1[x]] * fx, bottom = r1[b->mx0[x]] * (256 - fx) + r1[b->mx1[x]] * fx;
	unsigned v = (top * (256 - fy) + bottom * fy) >> 16;           /* 0..255 */

	return v + (v >> 7);                                            /* 0..256 */
}

static void
blend_rows(void *context, size_t piece)
{
	const struct blend *b = context;
	uint32_t rows = (b->height / 2 + 7) / 8, first = (uint32_t)piece * rows, end = first + rows;

	end = end > b->height / 2 ? b->height / 2 : end;
	for (uint32_t r2 = first; r2 < end; r2++) {
		for (uint32_t k = 0; k < 2; k++) {
			uint32_t y = r2 * 2 + k;
			uint8_t *row = b->y + y * b->y_stride;
			const uint8_t *s0 = b->small_y + b->sy0[y] * b->sw, *s1 = b->small_y + b->sy1[y] * b->sw;
			unsigned fy = b->syf[y];

			for (uint32_t x = 0; x < b->width; x++) {
				unsigned a = blur_at(b, x, y);

				if (b->show) {
					row[x] = (uint8_t)(16 + (219 * (256 - a) >> 8));
					continue;
				}
				if (a == 0) {
					continue;
				}
				unsigned fx = b->sxf[x];
				unsigned top = s0[b->sx0[x]] * (256 - fx) + s0[b->sx1[x]] * fx, bottom = s1[b->sx0[x]] * (256 - fx) + s1[b->sx1[x]] * fx;
				unsigned blurred = (top * (256 - fy) + bottom * fy) >> 16;

				row[x] = (uint8_t)((row[x] * (256 - a) + blurred * a) >> 8);
			}
		}
		/* the colour, at half the size each way: by the pixel at its top left */
		uint32_t y = r2 * 2;
		uint8_t *row = b->uv + r2 * b->uv_stride;
		const uint8_t *s0 = b->small_uv + b->sy0[y] * b->sw * 2, *s1 = b->small_uv + b->sy1[y] * b->sw * 2;
		unsigned fy = b->syf[y];

		for (uint32_t x2 = 0; x2 < b->width / 2; x2++) {
			uint32_t x = x2 * 2;
			unsigned a = blur_at(b, x, y);

			if (b->show) {
				row[x2 * 2] = row[x2 * 2 + 1] = 128;
				continue;
			}
			if (a == 0) {
				continue;
			}
			for (int c = 0; c < 2; c++) {
				unsigned fx = b->sxf[x];
				unsigned top = s0[b->sx0[x] * 2 + c] * (256 - fx) + s0[b->sx1[x] * 2 + c] * fx;
				unsigned bottom = s1[b->sx0[x] * 2 + c] * (256 - fx) + s1[b->sx1[x] * 2 + c] * fx;
				unsigned blurred = (top * (256 - fy) + bottom * fy) >> 16;

				row[x2 * 2 + c] = (uint8_t)((row[x2 * 2 + c] * (256 - a) + blurred * a) >> 8);
			}
		}
	}
}

/* a box of 5 along the rows then down the columns, `passes` times; `ch` channels interleaved */
static void
box_blur(uint8_t *p, int w, int h, int ch, int passes)
{
	uint8_t *line = malloc((size_t)(w > h ? w : h) * (size_t)ch);

	for (int pass = 0; pass < passes; pass++) {
		for (int y = 0; y < h; y++) {
			uint8_t *row = p + y * w * ch;

			memcpy(line, row, (size_t)(w * ch));
			for (int x = 0; x < w; x++) {
				for (int c = 0; c < ch; c++) {
					unsigned s = 0;

					for (int i = -2; i <= 2; i++) {
						int xi = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i;

						s += line[xi * ch + c];
					}
					row[x * ch + c] = (uint8_t)(s / 5);
				}
			}
		}
		for (int x = 0; x < w; x++) {
			for (int y = 0; y < h; y++) {
				for (int c = 0; c < ch; c++) {
					line[y * ch + c] = p[(y * w + x) * ch + c];
				}
			}
			for (int y = 0; y < h; y++) {
				for (int c = 0; c < ch; c++) {
					unsigned s = 0;

					for (int i = -2; i <= 2; i++) {
						int yi = y + i < 0 ? 0 : y + i >= h ? h - 1 : y + i;

						s += line[yi * ch + c];
					}
					p[(y * w + x) * ch + c] = (uint8_t)(s / 5);
				}
			}
		}
	}
	free(line);
}

/* index, next and fraction (256ths) of position v in 0..n-1 */
static void
lerp_at(float v, int n, int32_t *i0, int32_t *i1, uint8_t *f)
{
	int i = (int)floorf(v);
	float fr = v - i;

	if (i < 0) {
		i = 0, fr = 0;
	}
	if (i >= n - 1) {
		i = n - 1, fr = 0;
	}
	*i0 = i;
	*i1 = i + 1 < n ? i + 1 : i;
	*f = (uint8_t)(fr * 255 + 0.5f);
}

bool
portrait_apply(uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, float zoom,
    float fx, float fy, bool show)
{
	static uint8_t map_copy[MH][MW];
	static uint8_t *small_y, *small_uv;
	static int32_t *tabs;
	static uint8_t *fracs;
	static uint32_t tabs_for;
	const float sensor_aspect = 4.0f / 3;
	float aspect = (float)width / height, span_x = aspect > sensor_aspect ? 1 : aspect / sensor_aspect;
	float span_y = aspect > sensor_aspect ? sensor_aspect / aspect : 1;

	if (width < 16 || height < 16 || (width | height) & 1) {
		return false;
	}
	zoom = zoom < 1 ? 1 : zoom;
	if (width >= 2000) {
		still_zoom = zoom;              /* (a photograph: its depth is made later, by portrait_disparity, at this zoom) */
	}
	/* where the focus is in W */
	focus_u = 0.5f + (fx - 0.5f) * span_x / zoom * fit_r;
	focus_v = 0.5f + (fy - 0.5f) * span_y / zoom * fit_r;
	focus_u = focus_u < 0 ? 0 : focus_u > 0.999f ? 0.999f : focus_u;
	focus_v = focus_v < 0 ? 0 : focus_v > 0.999f ? 0.999f : focus_v;

	pthread_mutex_lock(&map_lock);
	bool have = have_map;

	if (have) {
		memcpy(map_copy, map, sizeof(map_copy));
	}
	pthread_mutex_unlock(&map_lock);
	if (!have) {
		return false;
	}

	/* the blurred picture: the picture at 1/f its size (f even, about 240 across), a box of 5 three times over */
	uint32_t f = (uint32_t)(width / 240.0f + 0.5f) & ~1u;

	f = f < 2 ? 2 : f;
	uint32_t sw = width / f, sh = height / f;

	if (sw > SMALL_MAX || sh > SMALL_MAX) {
		return false;
	}
	if (small_y == NULL) {
		small_y = malloc(SMALL_MAX * SMALL_MAX);
		small_uv = malloc(SMALL_MAX * SMALL_MAX * 2);
	}
	for (uint32_t j = 0; j < sh; j++) {
		for (uint32_t i = 0; i < sw; i++) {
			unsigned s = 0, su = 0, sv = 0;

			for (uint32_t b = 0; b < f; b++) {
				const uint8_t *row = y + (j * f + b) * y_stride + i * f;

				for (uint32_t a = 0; a < f; a++) {
					s += row[a];
				}
			}
			for (uint32_t b = 0; b < f / 2; b++) {
				const uint8_t *row = uv + (j * f / 2 + b) * uv_stride + i * f;

				for (uint32_t a = 0; a < f / 2; a++) {
					su += row[a * 2];
					sv += row[a * 2 + 1];
				}
			}
			small_y[j * sw + i] = (uint8_t)(s / (f * f));
			small_uv[(j * sw + i) * 2] = (uint8_t)(su / (f * f / 4));
			small_uv[(j * sw + i) * 2 + 1] = (uint8_t)(sv / (f * f / 4));
		}
	}
	box_blur(small_y, (int)sw, (int)sh, 1, 3);
	box_blur(small_uv, (int)sw, (int)sh, 2, 3);

	/* the tables */
	if (tabs_for < width + height) {
		free(tabs);
		free(fracs);
		tabs_for = width + height;
		tabs = malloc(sizeof(int32_t) * 4 * tabs_for);
		fracs = malloc(2 * tabs_for);
	}
	struct blend b = { y, uv, y_stride, uv_stride, width, height, sw, sh, small_y, small_uv,
		tabs, tabs + width, tabs + 2 * width, tabs + 3 * width, fracs, fracs + width,
		tabs + 4 * width, tabs + 4 * width + height, tabs + 4 * width + 2 * height, tabs + 4 * width + 3 * height,
		fracs + 2 * width, fracs + 2 * width + height, (const uint8_t (*)[MW])map_copy, show };

	for (uint32_t x = 0; x < width; x++) {
		float u = 0.5f + ((x + 0.5f) / width - 0.5f) * span_x / zoom * fit_r;

		lerp_at((x + 0.5f) / f - 0.5f, (int)sw, &b.sx0[x], &b.sx1[x], &b.sxf[x]);
		lerp_at(u * MW - 0.5f, MW, &b.mx0[x], &b.mx1[x], &b.mxf[x]);       /* (beyond the map: its edge's) */
	}
	for (uint32_t j = 0; j < height; j++) {
		float v = 0.5f + ((j + 0.5f) / height - 0.5f) * span_y / zoom * fit_r;

		lerp_at((j + 0.5f) / f - 0.5f, (int)sh, &b.sy0[j], &b.sy1[j], &b.syf[j]);
		lerp_at(v * MH - 0.5f, MH, &b.my0[j], &b.my1[j], &b.myf[j]);
	}
	dispatch_apply_f(8, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &b, blend_rows);
	return true;
}

/*
 * The photograph's depth, as Apple's photographs keep it: disparity (1/metres), half floats, width x height over the
 * picture portrait_apply last blurred as a photograph. Parallax is K / distance with K about 4.8 pixels of T and metres
 * (boot 20: 16 pixels at 30 cm), counted from the cameras' known line-up, whose own distance is not known and is taken as
 * 2 m (REF_DISPARITY). Outside what both cameras see, as at the nearest edge of what they do.
 */
#define PARALLAX_K      4.8f
#define REF_DISPARITY   0.5f

bool
portrait_disparity(uint32_t width, uint32_t height, void *out, size_t stride)
{
	static float near_copy[MH][MW];
	const float sensor_aspect = 4.0f / 3;
	float aspect = (float)width / height, span_x = aspect > sensor_aspect ? 1 : aspect / sensor_aspect;
	float span_y = aspect > sensor_aspect ? sensor_aspect / aspect : 1, farthest = 0, zoom = still_zoom;

	pthread_mutex_lock(&map_lock);
	bool have = have_map;

	if (have) {
		memcpy(near_copy, near_f, sizeof(near_copy));
	}
	pthread_mutex_unlock(&map_lock);
	if (!have || width == 0 || height == 0) {
		return false;
	}
	for (int y = 0; y < MH; y++) {
		for (int x = 0; x < MW; x++) {
			farthest = near_copy[y][x] < farthest ? near_copy[y][x] : farthest;
		}
	}
	for (uint32_t j = 0; j < height; j++) {
		__fp16 *row = (__fp16 *)((uint8_t *)out + j * stride);
		float v = 0.5f + ((j + 0.5f) / height - 0.5f) * span_y / zoom * fit_r;

		for (uint32_t i = 0; i < width; i++) {
			float u = 0.5f + ((i + 0.5f) / width - 0.5f) * span_x / zoom * fit_r, n = farthest, d;

			{
				/* (beyond the map: its edge's, as the blur) */
				float uc = u < 0 ? 0 : u > 0.999f ? 0.999f : u, vc = v < 0 ? 0 : v > 0.999f ? 0.999f : v;
				float fx = uc * MW - 0.5f, fy = vc * MH - 0.5f;
				int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
				float ax = fx - x0, ay = fy - y0;
				int x1 = x0 + 1 >= MW ? MW - 1 : x0 + 1, y1 = y0 + 1 >= MH ? MH - 1 : y0 + 1;

				x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
				n = (near_copy[y0][x0] * (1 - ax) + near_copy[y0][x1] * ax) * (1 - ay) +
				    (near_copy[y1][x0] * (1 - ax) + near_copy[y1][x1] * ax) * ay;
			}
			d = REF_DISPARITY + n / PARALLAX_K;
			row[i] = (__fp16)(d < 0.05f ? 0.05f : d);
		}
	}
	return true;
}

#ifndef PORTRAIT_PHOTO
/* how the cameras line up and where the focus is, for the photograph's depth (camera_portrait_photo.c) */
bool
portrait_lined_up(float *r, int *o, int *dx, int *dy, float *fu, float *fv)
{
	*r = fit_r, *o = fit_o, *dx = fit_dx, *dy = fit_dy, *fu = focus_u, *fv = focus_v;
	return fitted;
}
#else
/*
 * The photograph's depth, now (on the caller's thread): from the wide's and the telephoto's views of the same moment, as
 * the preview's, at PSCALE times the size; how the cameras line up and where the focus is taken from the preview's.
 */
bool
portrait_photo_depth(const uint8_t *wide, const uint8_t *tele)
{
	float r, fu, fv;
	int o, dx, dy;

	if (!portrait_lined_up(&r, &o, &dx, &dy, &fu, &fv)) {
		return false;
	}
	fit_r = r, fit_o = o, fit_dx = dx, fit_dy = dy, fitted = true;
	focus_u = fu, focus_v = fv;
	memcpy(wide_job, wide, sizeof(wide_job));
	memcpy(tele_job, tele, sizeof(tele_job));
	pthread_mutex_lock(&map_lock);
	have_map = false;
	pthread_mutex_unlock(&map_lock);
	acc_runs = 0;
	depth();
	return true;
}
#endif
