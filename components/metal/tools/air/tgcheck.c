/*
 * tgcheck: tools/air/threadgroup_both.ll dispatched through libhakios-air (tools/air/tgcheck.sh): sixteen
 * threads write their index + 100 into the kernel's own threadgroup array and their index + 1000 into the
 * threadgroup buffer the dispatch sizes, and each reads back the next thread's two. Both read back right
 * only if the two memories are kept apart -- the buffer was put at address 0, over the kernel's own array,
 * and such a kernel was refused at the dispatch. With `two`, tools/air/threadgroup_two.ll: a second buffer
 * (index 1) beside them, + 2000, which reads back right only if it starts where buffer 0 ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	int two = argc > 2, k = two ? 3 : 2;
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = rwa_compute_create(d, bc, n, two ? "tg2" : "tg", &info, why, sizeof(why));
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_buffer *b = rwa_buffer_create(d, 192, why, sizeof(why));
	memset(rwa_buffer_map(b), 0, 192);
	rwa_threadgroup_memory(c, two ? 68 : 64);
	if (two)
		rwa_threadgroup_memory_at(c, 1, 64);
	struct rwa_buffer *bufs[1] = { b };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, size[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, size, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned *o = rwa_buffer_map(b); int bad = 0;
	for (unsigned t = 0; t < 16; t++) {
		unsigned nx = (t + 1) & 15;
		if (o[k * t] != nx + 100 || o[k * t + 1] != nx + 1000 || (two && o[k * t + 2] != nx + 2000)) {
			bad++;
			printf("thread %u: %u %u %u\n", t, o[k*t], o[k*t+1], two ? o[k*t+2] : 0);
		}
	}
	printf("%s\n", bad ? "WRONG" : two ? "two threadgroup buffers and the kernel's own kept apart: ok"
	                                 : "both threadgroup memories kept apart: ok");
	return bad != 0;
}
