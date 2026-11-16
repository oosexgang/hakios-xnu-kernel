/*
 * bufcmpcheck: tools/air/buffer_compare.ll dispatched through libhakios-air (tools/air/bufcmpcheck.sh).
 * The kernel compares its two buffer arguments: two buffers say "different", one buffer bound at
 * both indices says "the same", and one buffer bound at index 1 four bytes further in says the
 * second pointer, dst + 1, is src. The compute state is folded per dispatch, so all three run on
 * one rwa_compute. Then tools/air/pointer_walk.ll, a loop over a local array that stops at its
 * end pointer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"

static int run(struct rwa_compute *c, struct rwa_buffer *b0, struct rwa_buffer *b1, size_t off1,
               unsigned want0, unsigned want1, const char *what) {
	char why[512] = "";
	struct rwa_buffer *b[2] = { b0, b1 };
	struct rwa_buffer_bind t[2] = { { b0, 0 }, { b1, off1 } };
	struct rwa_bindings bind = { .buffers = b, .buffer_count = 2 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { 1, 1, 1 };
	unsigned *o = rwa_buffer_map(b0);
	memset(o, 0, 16);
	if (rwa_dispatch_buffers(c, &bind, t, 2, g, s, why, sizeof(why))) { printf("%s: %s\n", what, why); return 1; }
	o = rwa_buffer_map(b0);
	int bad = o[0] != want0 || o[1] != want1;
	printf("%s: %u %u%s\n", what, o[0], o[1], bad ? " WRONG" : "");
	return bad;
}

int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n); if (fread(bc, 1, n, f) != (size_t)n) return 1; fclose(f);
	char why[512] = ""; struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info; struct rwa_compute *c = rwa_compute_create(d, bc, n, "bc", &info, why, sizeof(why));
	if (!c) { printf("%s\n", why); return 1; }
	struct rwa_buffer *x = rwa_buffer_create(d, 64, why, sizeof(why)), *y = rwa_buffer_create(d, 64, why, sizeof(why));
	int bad = run(c, x, y, 0, 2, 4, "two buffers");
	bad += run(c, x, x, 0, 1, 4, "one buffer twice");
	bad += run(c, x, x, 4, 2, 3, "one buffer, index 1 four bytes in");
	bad += run(c, x, y, 0, 2, 4, "two buffers again");
	f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	bc = malloc(n); if (fread(bc, 1, n, f) != (size_t)n) return 1; fclose(f);
	struct rwa_compute *w = rwa_compute_create(d, bc, n, "walk", &info, why, sizeof(why));
	if (!w) { printf("walk: %s\n", why); return 1; }
	struct rwa_buffer *b[2] = { x, y }; struct rwa_bindings bind = { .buffers = b, .buffer_count = 2 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { 1, 1, 1 };
	if (rwa_dispatch(w, &bind, g, s, why, sizeof(why))) { printf("walk: %s\n", why); return 1; }
	unsigned *o = rwa_buffer_map(x);
	printf("walk to an end pointer: %u%s\n", o[2], o[2] == 26 ? "" : " WRONG");
	bad += o[2] != 26;
	printf("%s\n", bad ? "WRONG" : "pointer compares: ok");
	return bad;
}
