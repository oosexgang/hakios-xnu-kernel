/*
 * fbfetchdraw: tools/air/fbfetch.ll's fbFetch drawn through libhakios-air (tools/air/fbfetchdraw.sh), the
 * draw path CoreAnimation's own goes through -- not air_run's. The target is cleared to (0.25, 0.5, 0.75, 1)
 * and drawn into twice, each draw doubling what it reads with a framebuffer fetch and adding (1, 2, 3, 4):
 * 1.5 3 4.5 6 after the first, 4 8 12 16 after the second. On softpipe the fetch is a texel fetch from a
 * copy of the attachment taken before the draw (sw_fbfetch_instr in air_metal.c). With "half" after the
 * file, fbFetchHalf into an RGBA16Float target, the format CoreAnimation's _lph shaders draw into.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { W = 8, H = 8, RGBA16F = 115, RGBA32F = 125, TEX2D = 2, READ = 1, RT = 4, TRIANGLE_STRIP = 4 };
static float half_to_float(unsigned short h) {
	unsigned e = h >> 10 & 31, m = h & 1023;
	float v = e == 0 ? m / 1024.0f / 16384.0f : (1 + m / 1024.0f) * (float)(1u << e) / 32768.0f;
	return h & 0x8000 ? -v : v;
}
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); size_t n = ftell(f); fseek(f, 0, SEEK_SET);
	void *fb = malloc(n);
	if (fread(fb, 1, n, f) != n) return 2;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!d) { printf("fbfetchdraw: %s\n", why); return 1; }
	int half = argc > 2 && !strcmp(argv[2], "half");
	unsigned format = half ? RGBA16F : RGBA32F;
	struct rwa_render_shape shape = { .color_format = { format }, .color_count = 1, .sample_count = 1 };
	struct rwa_render_info info;
	struct rwa_render *r = rwa_render_create(d, NULL, 0, NULL, fb, n, half ? "fbFetchHalf" : "fbFetch", &shape,
						 &info, why, sizeof(why));
	if (!r) { printf("fbfetchdraw: pipeline: %s\n", why); return 1; }
	struct rwa_texture_shape ts = { format, TEX2D, W, H, 1, 1, 1, 1, RT | READ };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	if (!t) { printf("fbfetchdraw: %s\n", why); return 1; }
	static const float clear[1][4] = { { 0.25f, 0.5f, 0.75f, 1 } };
	static const float want[2][4] = { { 1.5f, 3, 4.5f, 6 }, { 4, 8, 12, 16 } };
	int bad = 0;
	for (int k = 0; k < 2; k++) {
		struct rwa_draw dr;
		memset(&dr, 0, sizeof(dr));
		dr.color = &t;
		dr.color_count = 1;
		dr.clear_colors = clear;
		dr.clear_mask = k == 0;
		dr.primitive = TRIANGLE_STRIP;
		dr.vertex_count = 4;
		float px[H][W][4];
		unsigned short hx[H][W][4];
		unsigned origin[3] = { 0, 0, 0 }, size[3] = { W, H, 1 };
		if (rwa_render_draw(r, &dr, why, sizeof(why)) || rwa_finish(d, why, sizeof(why)) ||
		    rwa_texture_read(t, 0, 0, origin, size, half ? (void *)hx : (void *)px, W * (half ? 8 : 16), why,
				     sizeof(why))) {
			printf("fbfetchdraw: draw %d: %s\n", k + 1, why);
			return 1;
		}
		if (half)
			for (int c = 0; c < 4; c++)
				px[3][5][c] = half_to_float(hx[3][5][c]);
		for (int c = 0; c < 4; c++)
			if (px[3][5][c] != want[k][c]) {
				printf("fbfetchdraw: after draw %d, component %d is %g, want %g\n", k + 1, c, px[3][5][c], want[k][c]);
				bad++;
			}
	}
	printf("fbfetchdraw: %s%s\n", bad ? "WRONG" : "a framebuffer fetch reads what the draws before it wrote: ok",
	       half ? " (halves)" : "");
	return bad != 0;
}
