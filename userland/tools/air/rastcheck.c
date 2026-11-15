/*
 * rastcheck: tools/air/rast_tri.ll and tools/air/rast_frag.ll drawn through libhakios-air (tools/air/rastcheck.sh)
 * under the raster states a Metal encoder sets: two triangles, the left one clockwise as seen and the right one
 * counter-clockwise, culled by each MTLCullMode under each MTLWinding -- which of the two is left is which way
 * round the library took each -- and filled as lines, which leaves each triangle's middle clear. Then both
 * moved to z = -0.5, outside Metal's depth range: clipped away under MTLDepthClipModeClip, drawn under Clamp.
 * The pass has a depth attachment (and no depth test), which is when the library clips to Metal's [0, 1]
 * rather than OpenGL's [-1, 1] (rwa_render_create's clip_halfz).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { W = 32, H = 16, RGBA32F = 125, DEPTH32F = 252, TEX2D = 2, READ = 1, RT = 4, TRIANGLES = 3 };
static void *load(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	void *b = malloc(*n);
	if (fread(b, 1, *n, f) != *n) { fclose(f); return NULL; }
	fclose(f);
	return b;
}
static const float corners[6][4] = {
	{ -0.9f, -0.8f, 0, 1 }, { -0.5f, 0.8f, 0, 1 }, { -0.1f, -0.8f, 0, 1 },   /* clockwise as seen */
	{ 0.1f, -0.8f, 0, 1 }, { 0.9f, -0.8f, 0, 1 }, { 0.5f, 0.8f, 0, 1 },      /* counter-clockwise */
};
static struct rwa_device *d;
static struct rwa_render *r;
static struct rwa_buffer *cb;
static struct rwa_texture *t, *z;
static float px[H][W][4];
static int draw(unsigned cull, unsigned winding, unsigned fill, unsigned clamp, char *why) {
	static const float black[1][4] = { { 0, 0, 0, 0 } };
	struct rwa_buffer *vb[1] = { cb };
	struct rwa_draw dr;
	memset(&dr, 0, sizeof(dr));
	dr.color = &t;
	dr.color_count = 1;
	dr.depth = z;
	dr.clear_colors = black;
	dr.clear_mask = 1;
	dr.primitive = TRIANGLES;
	dr.vertex_count = 6;
	dr.vertex.buffers = vb;
	dr.vertex.buffer_count = 1;
	dr.cull_mode = cull;
	dr.winding = winding;
	dr.fill_mode = fill;
	dr.depth_clamp = clamp;
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { W, H, 1 };
	return rwa_render_draw(r, &dr, why, 512) || rwa_finish(d, why, 512) ||
	       rwa_texture_read(t, 0, 0, origin, size, px, W * 16, why, 512);
}
/* the middle of the left triangle and of the right one */
static int lit(int x, int y) { return px[y][x][0] > 0.5f; }
int main(int argc, char **argv) {
	size_t vn, fn;
	void *vb = load(argv[1], &vn), *fb = load(argv[2], &fn);
	char why[512] = "";
	d = rwa_device_open(why, sizeof(why));
	if (!vb || !fb || !d) { printf("setup: %s\n", why); return 1; }
	struct rwa_render_shape shape = { .color_format = { RGBA32F }, .color_count = 1, .depth_format = DEPTH32F,
					  .sample_count = 1 };
	struct rwa_render_info info;
	r = rwa_render_create(d, vb, vn, "rv", fb, fn, "rf", &shape, &info, why, sizeof(why));
	if (!r) { printf("pipeline: %s\n", why); return 1; }
	cb = rwa_buffer_create(d, sizeof(corners), why, sizeof(why));
	memcpy(rwa_buffer_map(cb), corners, sizeof(corners));
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, W, H, 1, 1, 1, 1, RT | READ };
	t = rwa_texture_create(d, &ts, why, sizeof(why));
	struct rwa_texture_shape zs = { DEPTH32F, TEX2D, W, H, 1, 1, 1, 1, RT };
	z = t ? rwa_texture_create(d, &zs, why, sizeof(why)) : NULL;
	if (!t || !z || !cb) { printf("setup: %s\n", why); return 1; }
	/* the triangles' middles: x 8 and 24 of 32, y 9 of 16 (a third of the way up) */
	static const struct { const char *what; unsigned cull, winding, fill, clamp; float z; int left, right; } k[] = {
		{ "no culling", 0, 0, 0, 0, 0, 1, 1 },
		{ "back faces culled, clockwise in front", 2, 0, 0, 0, 0, 1, 0 },
		{ "back faces culled, counter-clockwise in front", 2, 1, 0, 0, 0, 0, 1 },
		{ "front faces culled, clockwise in front", 1, 0, 0, 0, 0, 0, 1 },
		{ "front faces culled, counter-clockwise in front", 1, 1, 0, 0, 0, 1, 0 },
		{ "filled as lines", 0, 0, 1, 0, 0, 0, 0 },
		{ "at z -0.5, depth clipped", 0, 0, 0, 0, -0.5f, 0, 0 },
		{ "at z -0.5, depth clamped", 0, 0, 0, 1, -0.5f, 1, 1 },
	};
	int bad = 0;
	for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		float *c = rwa_buffer_map(cb);
		for (int v = 0; v < 6; v++) c[4 * v + 2] = k[i].z;
		if (draw(k[i].cull, k[i].winding, k[i].fill, k[i].clamp, why)) { printf("%s: %s\n", k[i].what, why); return 1; }
		int l = lit(8, 9), rt = lit(24, 9);
		int ok = l == k[i].left && rt == k[i].right;
		/* lines: the middles are clear but the edges are drawn */
		if (k[i].fill) {
			int edges = 0;
			for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) edges += lit(x, y);
			ok = ok && edges > 0;
		}
		printf("%-48s left %s, right %s: %s\n", k[i].what, l ? "drawn" : "clear", rt ? "drawn" : "clear",
		       ok ? "ok" : "WRONG");
		bad += !ok;
	}
	return bad != 0;
}
