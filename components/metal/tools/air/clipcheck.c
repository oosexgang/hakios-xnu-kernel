/*
 * clipcheck: tools/air/clip_tri.ll and tools/air/rast_frag.ll drawn through libhakios-air (tools/air/clipcheck.sh):
 * a quad over the whole target whose vertex shader writes two clip distances, x + 0.5 and 0.5 - y. Metal clips
 * by every clip distance written, so the quarter left of x = -0.5 and the quarter above y = 0.5 stay clear and
 * the rest is red.
 */
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
static const float corners[6][4] = {
	{ -1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, -1, 0, 1 },
	{ 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
};
static float px[H][W][4];
int main(int argc, char **argv) {
	size_t vn, fn;
	void *vb = load(argv[1], &vn), *fb = load(argv[2], &fn);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!vb || !fb || !d) { printf("setup: %s\n", why); return 1; }
	struct rwa_render_shape shape = { .color_format = { RGBA32F }, .color_count = 1, .sample_count = 1 };
	struct rwa_render_info info;
	struct rwa_render *r = rwa_render_create(d, vb, vn, "cv", fb, fn, "rf", &shape, &info, why, sizeof(why));
	if (!r) { printf("pipeline: %s\n", why); return 1; }
	struct rwa_buffer *cb = rwa_buffer_create(d, sizeof(corners), why, sizeof(why));
	memcpy(rwa_buffer_map(cb), corners, sizeof(corners));
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, W, H, 1, 1, 1, 1, RT | READ };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	if (!t || !cb) { printf("setup: %s\n", why); return 1; }
	static const float black[1][4] = { { 0, 0, 0, 0 } };
	struct rwa_buffer *vbs[1] = { cb };
	struct rwa_draw dr;
	memset(&dr, 0, sizeof(dr));
	dr.color = &t;
	dr.color_count = 1;
	dr.clear_colors = black;
	dr.clear_mask = 1;
	dr.primitive = TRIANGLES;
	dr.vertex_count = 6;
	dr.vertex.buffers = vbs;
	dr.vertex.buffer_count = 1;
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { W, H, 1 };
	if (rwa_render_draw(r, &dr, why, sizeof(why)) || rwa_finish(d, why, sizeof(why)) ||
	    rwa_texture_read(t, 0, 0, origin, size, px, W * 16, why, sizeof(why))) {
		printf("draw: %s\n", why);
		return 1;
	}
	/* x = -0.5 is column 8 of 32, y = 0.5 a quarter of the way down: row 4 of 16 (Metal's y is up) */
	int bad = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int want = x >= 8 && y >= 4, lit = px[y][x][0] > 0.5f;
			if (lit != want && (x != 7 && x != 8) && (y != 3 && y != 4))
				bad++;
		}
	}
	for (int y = 0; y < H; y += 3) {
		for (int x = 0; x < W; x++)
			putchar(px[y][x][0] > 0.5f ? '#' : '.');
		putchar('\n');
	}
	printf("%s\n", bad ? "WRONG" : "two clip distances: ok");
	return bad != 0;
}
