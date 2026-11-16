/*
 * texarraycheck: tools/air/tex_array.ll dispatched through libhakios-air (tools/air/texarraycheck.sh): two
 * textures put in a local array and read in a loop at the index it counts. Texture 0 holds 1 + x, texture
 * 1 holds 100 + x, and thread t writes texel (t, 0) of each to out[2t] and out[2t + 1]. With `int`,
 * tools/air/tex_int.ll: texture 1 through an integer stored in a local and read back in a callee, out[2t + 1]
 * only. With `sel`, tools/air/tex_sel.ll: texture 1 for odd threads and texture 0 for even ones, chosen
 * between two members of an argument buffer and read through an integer, out[2t + 1] only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { RGBA32F = 125, TEX2D = 2, READ = 1, W = 4 };
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	int as_sel = argc > 2 && !strcmp(argv[2], "sel");
	int as_int = as_sel || (argc > 2 && !strcmp(argv[2], "int"));
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, as_sel ? "ts" : as_int ? "ti" : "ta", &info, why,
							  sizeof(why)) : NULL;
	if (!c) { printf("texarraycheck: %s\n", why); return 1; }
	struct rwa_texture_shape ts = { RGBA32F, TEX2D, W, 1, 1, 1, 1, 1, READ };
	struct rwa_texture *texs[2];
	for (int k = 0; k < 2; k++) {
		float texels[W][4];
		for (int x = 0; x < W; x++)
			texels[x][0] = texels[x][1] = texels[x][2] = texels[x][3] = (k ? 100 : 1) + x;
		unsigned o[3] = { 0, 0, 0 }, s[3] = { W, 1, 1 };
		texs[k] = rwa_texture_create(d, &ts, why, sizeof(why));
		if (!texs[k] || rwa_texture_write(texs[k], 0, 0, o, s, texels, sizeof(texels), why, sizeof(why))) {
			printf("texarraycheck: %s\n", why);
			return 1;
		}
	}
	struct rwa_buffer *out = rwa_buffer_create(d, 64, why, sizeof(why));
	memset(rwa_buffer_map(out), 0, 64);
	struct rwa_buffer *bufs[1] = { out };
	struct rwa_bindings bind = { .textures = texs, .texture_count = 2, .buffers = bufs, .buffer_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, gs[3] = { W, 1, 1 };
	if (as_sel) {
		/* the two textures encoded into the set at ids 0 and 1, the set bound at buffer 0 */
		struct rwa_buffer *set = rwa_buffer_create(d, 16, why, sizeof(why));
		if (!set || rwa_argbuf_encode_texture(set, 0, 0, texs[0], why, sizeof(why)) ||
		    rwa_argbuf_encode_texture(set, 0, 1, texs[1], why, sizeof(why))) {
			printf("texarraycheck: %s\n", why);
			return 1;
		}
		struct rwa_buffer_bind table[2] = { { set, 0 }, { out, 0 } };
		struct rwa_buffer *both[2] = { set, out };
		struct rwa_bindings sb = { .buffers = both, .buffer_count = 2 };
		if (rwa_dispatch_buffers(c, &sb, table, 2, groups, gs, why, sizeof(why))) {
			printf("texarraycheck: %s\n", why);
			return 1;
		}
	} else if (rwa_dispatch(c, &bind, groups, gs, why, sizeof(why))) {
		printf("texarraycheck: %s\n", why);
		return 1;
	}
	const float *o = rwa_buffer_map(out);
	int bad = 0;
	for (int t = 0; t < W; t++)
		for (int i = 0; i < 2; i++) {
			float want = as_int && !i ? 0 : ((as_sel ? t & 1 : i) ? 100 : 1) + t;
			if (o[2 * t + i] != want) {
				printf("texarraycheck: out[%d] is %g, want %g\n", 2 * t + i, o[2 * t + i], want);
				bad++;
			}
		}
	printf("texarraycheck: %s: %s\n",
	       as_sel ? "a texture chosen between two members of an argument buffer, read through an integer"
	       : as_int ? "a texture through an integer in a local, read in a callee"
			: "textures out of a local array at a counted index", bad ? "WRONG" : "ok");
	return bad != 0;
}
