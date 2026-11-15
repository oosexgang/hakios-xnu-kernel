/*
 * texatomcheck: tools/air/texture_atomic.ll dispatched through libhakios-air (tools/air/texatomcheck.sh): a
 * max and an add on two texels of an R32Uint texture from sixteen threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { R32UINT = 53, TEX2D = 2, READ = 1, WRITE = 2 };
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, "ta", &info, why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_texture_shape ts = { R32UINT, TEX2D, 4, 4, 1, 1, 1, 1, READ | WRITE };
	struct rwa_texture *t = rwa_texture_create(d, &ts, why, sizeof(why));
	if (!t) { printf("texture: %s\n", why); return 1; }
	unsigned zero[16] = { 0 }, origin[3] = { 0, 0, 0 }, size[3] = { 4, 4, 1 };
	if (rwa_texture_write(t, 0, 0, origin, size, zero, 16, why, sizeof(why))) { printf("write: %s\n", why); return 1; }
	struct rwa_buffer *b = rwa_buffer_create(d, 64, why, sizeof(why));
	memset(rwa_buffer_map(b), 0xff, 64);
	struct rwa_buffer *bufs[1] = { b };
	struct rwa_texture *texs[1] = { t };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 1, .textures = texs, .texture_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, gs[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, gs, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned texel[16];
	if (rwa_texture_read(t, 0, 0, origin, size, texel, 16, why, sizeof(why))) { printf("read: %s\n", why); return 1; }
	unsigned *was = rwa_buffer_map(b), seen = 0;
	for (int i = 0; i < 16; i++) if (was[i] < 16) seen |= 1u << was[i];
	int ok = texel[0] == 15 && texel[1] == 16 && seen == 0xffff;
	printf("texel (0,0) %u (want 15), (1,0) %u (want 16), the adds handed back %s: %s\n", texel[0], texel[1],
	       seen == 0xffff ? "0..15" : "something else", ok ? "ok" : "WRONG");
	return !ok;
}
