/*
 * argbufvalcheck: tools/air/argbuf_values.ll, tools/air/argbuf_atomic.ll or tools/air/argbuf_halfpad.ll (the entry point given) dispatched through libhakios-air (tools/air/argbufvalcheck.sh).
 * Sixteen threads read values out of a constant argument buffer -- scalars, a vector, an array indexed at
 * run time, a member of an inline struct -- and, in a device set, count themselves with an atomic and
 * scale by a value beside it; and a bool and a half with one padding byte between them. The set is bound as the buffer at its own index, as Metal binds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
static int run(struct rwa_device *d, void *bc, long n, const char *entry) {
	char why[512] = "";
	struct rwa_compute_info info; struct rwa_compute *c = rwa_compute_create(d, bc, n, entry, &info, why, sizeof(why));
	if (!c) { printf("%s: %s\n", entry, why); return 1; }
	struct rwa_buffer *set = rwa_buffer_create(d, 64, why, sizeof(why)), *out = rwa_buffer_create(d, 64, why, sizeof(why));
	float *v = rwa_buffer_map(set); memset(v, 0, 64);
	int av = !strcmp(entry, "av"), ap = !strcmp(entry, "ap");
	if (ap) {
		v[0] = 1000; ((unsigned char *)v)[4] = 1; ((unsigned short *)v)[3] = 0x3800; /* f, on, h = 0.5 */
	} else if (av) {
		v[0] = 1000; ((unsigned *)v)[1] = 200; v[2] = -1; v[3] = 30;     /* a, b, c */
		for (int k = 0; k < 2; k++) for (int j = 0; j < 4; j++) v[4 + 4 * k + j] = j == 2 ? 4 + k : -7;
		v[12] = -9; v[13] = 0.5f;                                          /* s.x, s.y */
	} else {
		v[1] = 3;                                                         /* n = 0, scale */
	}
	struct rwa_buffer *b[2] = { set, out };
	struct rwa_bindings bind = { .buffers = b, .buffer_count = 2 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, g, s, why, sizeof(why))) { printf("%s: %s\n", entry, why); return 1; }
	float *o = rwa_buffer_map(out); int bad = 0;
	for (int i = 0; i < 16; i++) {
		float e = ap ? 1000.5f + i : av ? 1000 + 200 + 30 + 4 + (i & 1) + 0.5f : 3.0f * i;
		if (o[i] != e) { bad++; printf("%s %d: %g want %g\n", entry, i, o[i], e); }
	}
	if (!av && !ap) {
		int cnt = ((int *)rwa_buffer_map(set))[0];
		if (cnt != 16) { bad++; printf("aa: the atomic counted %d threads, want 16\n", cnt); }
	}
	printf("%s: %s\n", entry, bad ? "WRONG" : ap ? "a half after a padding byte: ok" : av ? "values read out of a constant set: ok"
	                                           : "an atomic and a value in a device set: ok");
	return bad;
}
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = ""; struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!d) { printf("%s\n", why); return 1; }
	return run(d, bc, n, argv[2]);
}
