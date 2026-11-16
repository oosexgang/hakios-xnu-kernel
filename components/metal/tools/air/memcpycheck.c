/*
 * memcpycheck: tools/air/memcpy_runtime.ll dispatched through libhakios-air (tools/air/memcpycheck.sh) on one
 * thread with n = 37: the first 37 bytes of the destination must be the source's and the rest as they were.
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
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, "mc", &info, why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_buffer *dst = rwa_buffer_create(d, 64, why, sizeof(why)), *src = rwa_buffer_create(d, 64, why, sizeof(why));
	struct rwa_buffer *len = rwa_buffer_create(d, 16, why, sizeof(why));
	unsigned char *s = rwa_buffer_map(src);
	for (int i = 0; i < 64; i++) s[i] = (unsigned char)(i * 7 + 3);
	memset(rwa_buffer_map(dst), 0xee, 64);
	*(unsigned *)rwa_buffer_map(len) = 37;
	struct rwa_buffer *bufs[3] = { dst, src, len };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 3 };
	unsigned groups[3] = { 1, 1, 1 }, size[3] = { 1, 1, 1 };
	if (rwa_dispatch(c, &bind, groups, size, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned char *o = rwa_buffer_map(dst);
	int bad = 0;
	for (int i = 0; i < 64; i++) {
		unsigned want = i < 37 ? (unsigned char)(i * 7 + 3) : 0xee;
		if (o[i] != want) { if (bad++ < 5) printf("byte %d: %#x, want %#x\n", i, o[i], want); }
	}
	printf("a memcpy of 37 bytes the shader read: %s\n", bad ? "WRONG" : "ok");
	return bad != 0;
}
