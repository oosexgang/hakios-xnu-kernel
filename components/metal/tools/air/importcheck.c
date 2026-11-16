/*
 * importcheck: tools/air/callee_import.ll dispatched through libhakios-air (tools/air/importcheck.sh): sixteen
 * threads each write 3t through a struct passed to one callee and 5t + 1 through a pointer read back from a
 * local by another, at offsets only the caller computed.
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
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, "imp", &info, why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_buffer *b = rwa_buffer_create(d, 128, why, sizeof(why));
	memset(rwa_buffer_map(b), 0xff, 128);
	struct rwa_buffer *bufs[1] = { b };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 1 };
	unsigned groups[3] = { 1, 1, 1 }, size[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, size, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned *o = rwa_buffer_map(b), bad = 0;
	for (unsigned t = 0; t < 16; t++)
		if (o[2 * t] != 3 * t || o[2 * t + 1] != 5 * t + 1) { bad++; printf("thread %u: %u %u\n", t, o[2*t], o[2*t+1]); }
	printf("a caller's struct and local, read in callees: %s\n", bad ? "WRONG" : "ok");
	return bad != 0;
}
