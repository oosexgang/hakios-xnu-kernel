/*
 * tesscheck: tools/air/tess_quad.ll and tools/air/tess_frag.ll drawn through libhakios-air
 * (tools/air/tesscheck.sh) as a drawPatches of two quad patches would be: rwa_render_patch_vertices
 * vertices an instance, an instance a patch. Patch 0 must cover the left half of a 32 x 16 target and
 * patch 1 the right, every pixel, each with its own (u, v) and patch number -- a grid with a hole, a
 * position_in_patch off by a cell or a patch_id that is not the instance all show here. With `tri`,
 * tools/air/tess_tri.ll: one triangle patch over the lower-left half, each pixel its own barycentric. With
 * `cp`, tools/air/tess_cp.ll: the two quad patches again, their corners [[stage_in]] control points out of
 * a vertex buffer the vertex descriptor lays out, one float2 per control point.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { W = 32, H = 16, RGBA32F = 125, TEX2D = 2, READ = 1, RT = 4, TRIANGLES = 3 };
static void *load(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	void *b = malloc(*n);
	if (fread(b, 1, *n, f) != *n) { fclose(f); return NULL; }
	fclose(f);
	return b;
}
int main(int argc, char **argv) {
	size_t vn, fn;
	void *vb = load(argv[1], &vn), *fb = load(argv[2], &fn);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!vb || !fb || !d) { printf("setup: %s\n", why); return 1; }
	struct rwa_render_shape shape = { .color_format = { RGBA32F }, .color_count = 1, .sample_count = 1 };
	struct rwa_render_info info;
	int cp = argc > 3 && !strcmp(argv[3], "cp");
	int tri = argc > 3 && !cp;
	if (cp) {
		shape.vertex_attribute[0] = (struct rwa_vertex_attribute){ .format = 29, .offset = 0, .buffer_index = 0 };
		shape.vertex_layout[0] = (struct rwa_vertex_layout){ .stride = 8, .step_function = 4 };
	}
	struct rwa_render *r = rwa_render_create(d, vb, vn, tri ? "tt" : cp ? "tc" : "tv", fb, fn, "tf", &shape,
						 &info, why, sizeof(why));
	if (!r) { printf("pipeline: %s\n", why); return 1; }
	unsigned per_patch = rwa_render_patch_vertices(r);
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, W, H, 1, 1, 1, 1, RT | READ };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	if (!t || !per_patch) { printf("target: %s (%u vertices a patch)\n", why, per_patch); return 1; }
	static const float black[1][4] = { { 0, 0, 0, 0 } };
	struct rwa_draw dr;
	memset(&dr, 0, sizeof(dr));
	dr.color = &t;
	dr.color_count = 1;
	dr.clear_colors = black;
	dr.clear_mask = 1;
	dr.primitive = TRIANGLES;
	dr.vertex_count = per_patch;
	dr.instance_count = tri ? 1 : 2;
	/* the control points: two patches' corners, bottom-left, bottom-right, top-left, top-right */
	static const float corners[8][2] = { { -1, -1 }, { 0, -1 }, { -1, 1 }, { 0, 1 },
					      { 0, -1 }, { 1, -1 }, { 0, 1 }, { 1, 1 } };
	struct rwa_buffer *cb = NULL;
	struct rwa_vertex_stream streams[1];
	if (cp) {
		cb = rwa_buffer_create(d, sizeof(corners), why, sizeof(why));
		if (!cb) { printf("buffer: %s\n", why); return 1; }
		memcpy(rwa_buffer_map(cb), corners, sizeof(corners));
		streams[0] = (struct rwa_vertex_stream){ cb, 0 };
		dr.vertex_streams = streams;
		dr.vertex.buffers = &cb;
		dr.vertex.buffer_count = 1;
	}
	if (rwa_render_draw(r, &dr, why, sizeof(why)) || rwa_finish(d, why, sizeof(why))) {
		printf("draw: %s\n", why);
		return 1;
	}
	static float px[H][W][4];
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { W, H, 1 };
	if (rwa_texture_read(t, 0, 0, origin, size, px, W * 16, why, sizeof(why))) { printf("read: %s\n", why); return 1; }
	int bad = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float *c = px[y][x];
			if (tri) {
				/* the pixel centre in clip space, and its barycentrics over A B C */
				float cx = -1 + 2 * (x + 0.5f) / W, cy = 1 - 2 * (y + 0.5f) / H;
				float w1 = (cx + 1) / 2, w2 = (cy + 1) / 2, w0 = 1 - w1 - w2;
				if (w0 < 0.02f) {
					/* outside, or on the edge: left alone, or covered; either is right */
					if (w0 < -0.02f && (c[0] || c[1] || c[2] || c[3]) && bad++ < 6)
						printf("pixel (%d, %d) outside the patch was drawn\n", x, y);
					continue;
				}
				if (fabsf(c[0] - w0) > 0.03f || fabsf(c[1] - w1) > 0.03f ||
				    fabsf(c[2] - w2) > 0.03f || c[3] != 1) {
					if (bad++ < 6)
						printf("pixel (%d, %d): %.3f %.3f %.3f %.3f, want %.3f %.3f %.3f 1\n",
						       x, y, c[0], c[1], c[2], c[3], w0, w1, w2);
				}
				continue;
			}
			/* the pixel centre in the patch: x in [0, 16) of a half, y up from the bottom */
			float u = ((x % (W / 2)) + 0.5f) / (W / 2), v = 1.0f - (y + 0.5f) / H;
			float p = x >= W / 2;
			if (fabsf(c[0] - u) > 0.02f || fabsf(c[1] - v) > 0.02f || c[2] != p || c[3] != 1) {
				if (bad++ < 6)
					printf("pixel (%d, %d): %.3f %.3f %.3f %.3f, want %.3f %.3f %.0f 1\n", x, y,
					       c[0], c[1], c[2], c[3], u, v, p);
			}
		}
	printf("%s of %u vertices, %d x %d pixels: %s\n", tri ? "a triangle patch" :
	       cp ? "two quad patches of [[stage_in]] control points" : "two quad patches",
	       per_patch, W, H, bad ? "WRONG" : "ok");
	return bad != 0;
}
