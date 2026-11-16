/*
 * mipwritecheck: tools/air/mip_write.ll dispatched through libhakios-air (tools/air/mipwritecheck.sh): one
 * texture written at levels 0 and 1 by one kernel; both levels must hold what was written to them. With
 * `buffer`, tools/air/mip_buffer.ll: the same two levels, read out of a constant buffer that holds 1 (the
 * kernel writes at it and at it minus one). With `read`, tools/air/mip_read.ll: level 1 read and written
 * back at the level the buffer holds, plus 10 in blue, over levels filled first; level 0 stays as filled.
 * With `float`, tools/air/mip_float.ll: level 1 as (ushort)(a float the buffer holds, 0.0, + 1.0).
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
	int read_back = argc > 2 && !strcmp(argv[2], "read");
	int as_float = argc > 2 && !strcmp(argv[2], "float");
	int from_buffer = read_back || as_float || (argc > 2 && !strcmp(argv[2], "buffer"));
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, as_float ? "mf" : read_back ? "mr" : from_buffer ? "mb" : "mw", &info,
						       why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, 4, 4, 1, 1, 2, 1, READ | WRITE };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	if (!t) { printf("texture: %s\n", why); return 1; }
	struct rwa_texture *texs[1] = { t };
	if (read_back) {
		/* what the kernel reads: level 0 as mw writes it, level 1 as its quarter does */
		static float f0[4][4][4], f1[2][2][4];
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++) {
				float v0[4] = { x, y, 0, 1 }, v1[4] = { x, y, 1, 1 };
				memcpy(f0[y][x], v0, 16);
				if (x < 2 && y < 2)
					memcpy(f1[y][x], v1, 16);
			}
		unsigned fo[3] = { 0, 0, 0 }, fs0[3] = { 4, 4, 1 }, fs1[3] = { 2, 2, 1 };
		if (rwa_texture_write(t, 0, 0, fo, fs0, f0, 64, why, sizeof(why)) ||
		    rwa_texture_write(t, 1, 0, fo, fs1, f1, 32, why, sizeof(why))) {
			printf("fill: %s\n", why);
			return 1;
		}
	}
	struct rwa_buffer *level = rwa_buffer_create(d, 16, why, sizeof(why));
	if (!level) { printf("buffer: %s\n", why); return 1; }
	if (as_float)
		*(float *)rwa_buffer_map(level) = 0.0f;
	else
		*(unsigned short *)rwa_buffer_map(level) = 1;
	struct rwa_buffer *bufs[1] = { level };
	struct rwa_bindings bind = { .textures = texs, .texture_count = 1,
				     .buffers = from_buffer ? bufs : NULL, .buffer_count = from_buffer ? 1 : 0 };
	unsigned groups[3] = { 1, 1, 1 }, gs[3] = { 4, 4, 1 };
	if (rwa_dispatch(c, &bind, groups, gs, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	static float l0[4][4][4], l1[2][2][4];
	unsigned o[3] = { 0, 0, 0 }, s0[3] = { 4, 4, 1 }, s1[3] = { 2, 2, 1 };
	if (rwa_texture_read(t, 0, 0, o, s0, l0, 64, why, sizeof(why)) ||
	    rwa_texture_read(t, 1, 0, o, s1, l1, 32, why, sizeof(why))) { printf("read: %s\n", why); return 1; }
	int bad = 0;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			float w0[4] = { x, y, 0, 1 };
			if (memcmp(l0[y][x], w0, 16) && bad++ < 8)
				printf("level 0 (%d, %d): %g %g %g %g\n", x, y, l0[y][x][0], l0[y][x][1], l0[y][x][2], l0[y][x][3]);
			if (x < 2 && y < 2) {
				float w1[4] = { x, y, read_back ? 11 : 1, 1 };
				if (memcmp(l1[y][x], w1, 16) && bad++ < 8)
					printf("level 1 (%d, %d): %g %g %g %g\n", x, y, l1[y][x][0], l1[y][x][1], l1[y][x][2], l1[y][x][3]);
			}
		}
	printf("%s: %s\n", as_float ? "one texture written at level 0 and at a float from a buffer plus one"
	       : read_back ? "one texture read and written at the level a buffer holds"
	       : from_buffer ? "one texture written at levels 0 and 1, read out of a buffer"
			     : "one texture written at levels 0 and 1", bad ? "WRONG" : "ok");
	return bad != 0;
}
