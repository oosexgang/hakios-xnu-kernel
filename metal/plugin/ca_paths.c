/*
 * CoreAnimation's path renderer and where its coverage mask can be other than 0: ca_paths.h says what and why.
 * (C that is also C++: the host's tests include this file.)
 */
#include "ca_paths.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static double px_of(const struct ca_paths_viewport *v, double ndc) { return v->x + (ndc + 1) * 0.5 * v->w; }
static double py_of(const struct ca_paths_viewport *v, double ndc) { return v->y + (1 - ndc) * 0.5 * v->h; }

static void
band_clear(struct ca_paths_map *map, unsigned b)
{
	map->lo[b] = INFINITY, map->hi[b] = -INFINITY;
}

int
ca_paths_begin(struct ca_paths_map *map, unsigned width, unsigned height, int cleared)
{
	const unsigned n = (height + CA_PATHS_BAND - 1) / CA_PATHS_BAND;

	map->bands = 0, map->width = width, map->height = height;
	map->lo = (float *)malloc((n ? n : 1) * sizeof(float));
	map->hi = (float *)malloc((n ? n : 1) * sizeof(float));
	if (map->lo == NULL || map->hi == NULL) {
		ca_paths_end(map);
		return 0;
	}
	map->bands = n;
	if (cleared) {
		for (unsigned b = 0; b < n; b++) band_clear(map, b);
	} else {
		ca_paths_fill(map);
	}
	return 1;
}

void
ca_paths_end(struct ca_paths_map *map)
{
	free(map->lo), free(map->hi);
	map->lo = map->hi = NULL, map->bands = 0;
}

void
ca_paths_fill(struct ca_paths_map *map)
{
	for (unsigned b = 0; b < map->bands; b++) map->lo[b] = 0, map->hi[b] = (float)map->width;
}

/* the pixels X0..X1, Y0..Y1 (either way round) into the map, a pixel more on each side of the columns and rows */
static int
map_add(struct ca_paths_map *map, double x0, double x1, double y0, double y1)
{
	if (!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1)) return 0;
	if (x0 > x1) { const double t = x0; x0 = x1, x1 = t; }
	if (y0 > y1) { const double t = y0; y0 = y1, y1 = t; }
	const float a = (float)(floor(x0) - 1), z = (float)(ceil(x1) + 1);
	const double b1 = floor((y1 + 1) / CA_PATHS_BAND);

	for (double b = dmax(0.0, floor((y0 - 1) / CA_PATHS_BAND)); b <= b1 && b < map->bands; b++) {
		const unsigned k = (unsigned)b;
		map->lo[k] = a < map->lo[k] ? a : map->lo[k], map->hi[k] = z > map->hi[k] ? z : map->hi[k];
	}
	return 1;
}

/* (the buffers are the caller's GPU memory, write-combined and not cached: each load a trip across the bus, so that
 * reading 1696 pieces' points float by float took 44% of the window server's time in its draw calls -- metal_sample=1,
 * logs/073901-run.log. What is read is copied out in one go first.) */
static void *
copy_out(const uint8_t *from, uint64_t bytes)
{
	void *to = malloc(bytes ? bytes : 1);

	if (to != NULL) memcpy(to, from, bytes);
	return to;
}

int
ca_paths_cover(struct ca_paths_map *map, enum ca_path_role role, unsigned shift, const struct ca_paths_viewport *vp, const float *m,
	       const float *size, const uint8_t *edges, uint64_t edge_bytes, unsigned instances, unsigned base)
{
	if (instances == 0) return 1;
	if (m == NULL || size == NULL || edges == NULL || (role != CA_PATH_RECT && role != CA_PATH_CUBIC) || shift > 8) return 0;
	const unsigned first = role == CA_PATH_RECT ? base : base >> shift, last = role == CA_PATH_RECT ? base + instances - 1 : (base + instances - 1) >> shift;
	const unsigned stride = role == CA_PATH_RECT ? 20 : 40;
	if (edge_bytes < (uint64_t)(last + 1) * stride) return 0;
	float M[16], pad[4];
	memcpy(M, m, sizeof(M)), memcpy(pad, size, sizeof(pad));
	float *e = (float *)copy_out(edges + (size_t)first * stride, (uint64_t)(last - first + 1) * stride);
	int ok = e != NULL;

	for (unsigned k = 0; ok && k <= last - first; k++) {
		if (role == CA_PATH_RECT) {
			/* (path_rect_vert: the corners through the matrix's diagonal, the rows padded) */
			const float *r = e + (size_t)k * 5;
			const double ya = r[1] * M[5] + M[13], yb = r[3] * M[5] + M[13];
			const double x0 = r[0] * M[0] + M[12], x1 = r[2] * M[0] + M[12];
			ok = map_add(map, px_of(vp, x0), px_of(vp, x1), py_of(vp, dmin(ya, yb) - pad[3]), py_of(vp, dmax(ya, yb) + pad[3]));
			continue;
		}
		/* (path_cubic_vert: each piece's two ends through the matrix; from the left one, padded, across to the right
		 * extent; the rows between the ends, padded) -- the cubic's points once, its pieces of this draw from them */
		const float *c = e + (size_t)k * 10;
		const unsigned pieces = 1u << shift, cubic = first + k;
		double ex[257], ey[257];
		for (unsigned q = 0; q <= pieces; q++) {
			const double t = (double)q / pieces;
			const double x = ((c[2] * t + c[4]) * t + c[6]) * t + c[8], y = ((c[3] * t + c[5]) * t + c[7]) * t + c[9];
			ex[q] = x * M[0] + y * M[4] + M[12], ey[q] = x * M[1] + y * M[5] + M[13];
		}
		const double right = c[1] * M[0] + M[12];
		for (unsigned q = 0; ok && q < pieces; q++) {
			const unsigned instance = (cubic << shift) + q;
			if (instance < base || instance >= base + instances) continue;
			const double left = dmin(ex[q], ex[q + 1]) - pad[2];
			ok = map_add(map, px_of(vp, dmin(left, right)), px_of(vp, dmax(left, right)), py_of(vp, dmin(ey[q], ey[q + 1]) - pad[3]),
				     py_of(vp, dmax(ey[q], ey[q + 1]) + pad[3]));
		}
	}
	free(e);
	return ok;
}

/* one of path_blit's rectangles in pixels, through a matrix that does not turn it */
static void
rect_px(const struct ca_paths_viewport *vp, const float *m, const float *r, double *x0, double *x1, double *y0, double *y1)
{
	const double xa = px_of(vp, r[0] * m[0] + m[12]), xb = px_of(vp, r[2] * m[0] + m[12]);
	const double ya = py_of(vp, r[1] * m[5] + m[13]), yb = py_of(vp, r[3] * m[5] + m[13]);
	*x0 = dmin(xa, xb), *x1 = dmax(xa, xb), *y0 = dmin(ya, yb), *y1 = dmax(ya, yb);
}

/* the pixels A..Z, Y0..Y1 back into the rectangle's own space, with SRC's colour, at OUT */
static void
rect_out(const struct ca_paths_viewport *vp, const float *m, const float *src, double a, double z, double y0, double y1, uint8_t *out)
{
	float *o = (float *)out;
	const double nx0 = ((a - vp->x) / vp->w * 2 - 1 - m[12]) / m[0], nx1 = ((z - vp->x) / vp->w * 2 - 1 - m[12]) / m[0];
	const double ny0 = ((1 - (y0 - vp->y) / vp->h * 2) - m[13]) / m[5], ny1 = ((1 - (y1 - vp->y) / vp->h * 2) - m[13]) / m[5];

	o[0] = (float)dmin(nx0, nx1), o[1] = (float)dmin(ny0, ny1), o[2] = (float)dmax(nx0, nx1), o[3] = (float)dmax(ny0, ny1);
	memcpy(o + 4, src + 4, 8);
}

int
ca_paths_cut(const struct ca_paths_map *map, const struct ca_paths_viewport *vp, const float *m, const uint8_t *rects, unsigned instances,
	     uint8_t *out, unsigned max)
{
	unsigned n = 0;

	if (m[1] != 0 || m[4] != 0 || m[0] == 0 || m[5] == 0 || vp->w <= 0 || vp->h <= 0) return -1;
	float *all = (float *)copy_out(rects, (uint64_t)instances * 24);
	if (all == NULL) return -1;
	for (unsigned i = 0; i < instances; i++) {
		const float *r = all + (size_t)i * 6;
		double X0, X1, Y0, Y1, ra = 0, rz = 0, ry0 = 0, ry1 = 0;
		int open = 0;

		rect_px(vp, m, r, &X0, &X1, &Y0, &Y1);
		if (!isfinite(X0) || !isfinite(X1) || !isfinite(Y0) || !isfinite(Y1)) {
			free(all);
			return -1;
		}
		for (double b = dmax(0.0, floor(Y0 / CA_PATHS_BAND)); b < map->bands && b * CA_PATHS_BAND < Y1; b++) {
			const unsigned k = (unsigned)b;
			const double a = dmax(X0, (double)map->lo[k]), z = dmin(X1, (double)map->hi[k]);
			const double ya = dmax(Y0, b * CA_PATHS_BAND), yz = dmin(Y1, (b + 1) * CA_PATHS_BAND);
			const int some = z > a && yz > ya;

			if (open && (!some || a != ra || z != rz)) {
				if (n == max) {
					free(all);
					return -1;
				}
				rect_out(vp, m, r, ra, rz, ry0, ry1, out + (size_t)n++ * 24);
				open = 0;
			}
			if (some && !open) open = 1, ra = a, rz = z, ry0 = ya;
			if (some) ry1 = yz;
		}
		if (open) {
			if (n == max) {
				free(all);
				return -1;
			}
			rect_out(vp, m, r, ra, rz, ry0, ry1, out + (size_t)n++ * 24);
		}
	}
	free(all);
	return (int)n;
}

void
ca_paths_zeroed(struct ca_paths_map *map, const struct ca_paths_viewport *vp, const float *m, const uint8_t *rects, unsigned instances,
		const unsigned *scissor)
{
	if (m[1] != 0 || m[4] != 0 || instances == 0) return;
	/* the rectangles in pixels, once (inside the scissor) */
	double *px = (double *)malloc((size_t)instances * 4 * sizeof(double));
	float *r = (float *)copy_out(rects, (uint64_t)instances * 24);
	if (px == NULL || r == NULL) {
		free(px), free(r);
		return;
	}
	for (unsigned i = 0; i < instances; i++) {
		double *p = px + (size_t)i * 4;
		rect_px(vp, m, r + (size_t)i * 6, &p[0], &p[1], &p[2], &p[3]);
		if (scissor != NULL) {
			p[0] = dmax(p[0], scissor[0]), p[1] = dmin(p[1], (double)scissor[0] + scissor[2]);
			p[2] = dmax(p[2], scissor[1]), p[3] = dmin(p[3], (double)scissor[1] + scissor[3]);
		}
	}
	for (unsigned b = 0; b < map->bands; b++) {
		if (!(map->hi[b] > map->lo[b])) continue;
		/* the band's columns on the target, at pixel centres (the map's extra pixel either side may lie past the edge,
		 * where no rectangle reaches) */
		const double from = dmax((double)map->lo[b], 0) + 0.5, to = dmin((double)map->hi[b], map->width) - 0.5;
		const double end = dmin((double)(b + 1) * CA_PATHS_BAND, map->height);
		int all = 1;

		/* each row's columns under the rectangles that cross it, row by row at pixel centres, as the rasterizer goes:
		 * CoreAnimation's rectangles of one path need not each span a row (no single one did, and every path_clear went
		 * over the whole screen, logs/075020-run.log), and where they meet they may miss each other by a hundred-thousandth
		 * of a pixel (137.846150 and 137.846161) with no pixel centre in between */
		for (double y = b * CA_PATHS_BAND + 0.5; all && y < end && to >= from; y++) {
			double at = from;
			int moved = 1;
			while (at <= to && moved) {
				moved = 0;
				for (unsigned i = 0; i < instances; i++) {
					const double *p = px + (size_t)i * 4;
					if (p[2] <= y && p[3] > y && p[0] <= at && p[1] > at) {
						/* (to the next pixel centre this rectangle does not reach) */
						at = ceil(p[1] - 0.5) + 0.5;
						moved = 1;
					}
				}
			}
			all = at > to;
		}
		if (all) band_clear(map, b);   /* (also a band only past the target's edge: no mask there) */
	}
	free(px), free(r);
}
