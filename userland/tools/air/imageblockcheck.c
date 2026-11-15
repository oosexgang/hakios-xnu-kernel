/*
 * imageblockcheck: tools/air/imageblock.ll dispatched through libhakios-air (tools/air/imageblockcheck.sh) as
 * one 4 x 4 group, an 8 x 8 RGBA32Float texture cleared to -1: the block's sixteen pixels must land at (2, 1)
 * to (5, 4), each (x, y, 7, 1) of the thread that wrote it, and nothing else may change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { RGBA32F = 125, TEX2D = 2, READ = 1, WRITE = 2 };
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, "ib", &info, why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, 8, 8, 1, 1, 1, 1, READ | WRITE };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	static float px[8][8][4];
	for (int i = 0; i < 8 * 8 * 4; i++) ((float *)px)[i] = -1;
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { 8, 8, 1 };
	if (!t || rwa_texture_write(t, 0, 0, origin, size, px, 8 * 16, why, sizeof(why))) { printf("texture: %s\n", why); return 1; }
	struct rwa_texture *texs[1] = { t };
	struct rwa_bindings bind = { .textures = texs, .texture_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, gs[3] = { 4, 4, 1 };
	if (rwa_dispatch(c, &bind, groups, gs, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	if (rwa_texture_read(t, 0, 0, origin, size, px, 8 * 16, why, sizeof(why))) { printf("read: %s\n", why); return 1; }
	int bad = 0;
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			int in = x >= 2 && x < 6 && y >= 1 && y < 5;
			float want[4] = { in ? x - 2 : -1, in ? y - 1 : -1, in ? 7 : -1, in ? 1 : -1 };
			if (memcmp(px[y][x], want, sizeof(want)) && bad++ < 5)
				printf("texel (%d, %d): %g %g %g %g, want %g %g %g %g\n", x, y, px[y][x][0], px[y][x][1],
				       px[y][x][2], px[y][x][3], want[0], want[1], want[2], want[3]);
		}
	printf("a 4 x 4 imageblock written out at (2, 1): %s\n", bad ? "WRONG" : "ok");
	return bad != 0;
}
