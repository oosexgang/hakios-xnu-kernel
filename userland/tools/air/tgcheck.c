/*
 * tgcheck: tools/air/threadgroup_both.ll dispatched through libhakios-air (tools/air/tgcheck.sh): sixteen
 * threads write their index + 100 into the kernel's own threadgroup array and their index + 1000 into the
 * threadgroup buffer the dispatch sizes, and each reads back the next thread's two. Both read back right
 * only if the two memories are kept apart -- the buffer was put at address 0, over the kernel's own array,
 * and such a kernel was refused at the dispatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n); fread(bc, 1, n, f); fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = rwa_compute_create(d, bc, n, "tg", &info, why, sizeof(why));
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_buffer *b = rwa_buffer_create(d, 128, why, sizeof(why));
	memset(rwa_buffer_map(b), 0, 128);
	rwa_threadgroup_memory(c, 64);
	struct rwa_buffer *bufs[1] = { b };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, size[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, size, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned *o = rwa_buffer_map(b); int bad = 0;
	for (unsigned t = 0; t < 16; t++) {
		unsigned nx = (t + 1) & 15;
		if (o[2 * t] != nx + 100 || o[2 * t + 1] != nx + 1000) { bad++; printf("thread %u: %u %u\n", t, o[2*t], o[2*t+1]); }
	}
	printf("%s\n", bad ? "WRONG" : "both threadgroup memories kept apart: ok");
	return bad != 0;
}
