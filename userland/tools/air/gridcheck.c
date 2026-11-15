/*
 * gridcheck: tools/air/grid_partial.ll dispatched through libhakios-air (tools/air/gridcheck.sh), once as
 * three whole groups of 16 and once as 37 threads in them (rwa_grid_threads), the last group partial. The
 * words past thread 36 must be left as they were, [[threads_per_grid]] must be 37 and the last group's
 * [[dispatch_threads_per_threadgroup]] 5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
static int run(struct rwa_device *d, struct rwa_compute *c, unsigned threads) {
	char why[512] = "";
	struct rwa_buffer *b = rwa_buffer_create(d, 256, why, sizeof(why));
	unsigned *o = rwa_buffer_map(b);
	for (int i = 0; i < 64; i++) o[i] = 0xdeadbeef;
	unsigned t[3] = { threads, 1, 1 };
	rwa_grid_threads(c, threads ? t : NULL);
	struct rwa_buffer *bufs[1] = { b };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 1 };
	unsigned groups[3] = { 3, 1, 1 }, size[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, size, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	o = rwa_buffer_map(b);
	unsigned n = threads ? threads : 48, bad = 0;
	for (unsigned i = 0; i < 64; i++) {
		unsigned group = i / 16, in_group = group < n / 16 ? 16 : n - 16 * group;
		unsigned want = i < n ? i + 1000 * n + 100000 * in_group : 0xdeadbeef;
		if (o[i] != want) { bad++; printf("word %u: %u, want %u\n", i, o[i], want); }
	}
	printf("%u threads in groups of 16: %s\n", n, bad ? "WRONG" : "ok");
	return bad != 0;
}
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = rwa_compute_create(d, bc, n, "grid", &info, why, sizeof(why));
	if (!c) { printf("compute: %s\n", why); return 1; }
	return run(d, c, 0) | run(d, c, 37) | run(d, c, 0);
}
