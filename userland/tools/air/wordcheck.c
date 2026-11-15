/*
 * wordcheck: one kernel of a .bc dispatched through libhakios-air on softpipe, buffer 0 a zeroed
 * 256 bytes and buffer 1 sixteen words 1000 + i, and buffer 0's first words compared with the ones
 * given (tools/air/wordcheck.sh):
 *
 *     wordcheck FILE.bc ENTRY THREADS WORD...
 *
 * A word is a decimal unsigned, or a float when it has a point in it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
int main(int argc, char **argv) {
	if (argc < 5) { fprintf(stderr, "usage: wordcheck FILE.bc ENTRY THREADS WORD...\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, argv[2], &info, why, sizeof(why)) : NULL;
	if (!c) { printf("%s: %s\n", argv[2], why); return 1; }
	struct rwa_buffer *b = rwa_buffer_create(d, 256, why, sizeof(why));
	memset(rwa_buffer_map(b), 0, 256);
	struct rwa_buffer *in = rwa_buffer_create(d, 64, why, sizeof(why));
	unsigned *words = rwa_buffer_map(in);
	for (unsigned i = 0; i < 16; i++)
		words[i] = 1000 + i;
	struct rwa_buffer *bufs[2] = { b, in };
	struct rwa_bindings bind = { .buffers = bufs, .buffer_count = 2 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { (unsigned)atoi(argv[3]), 1, 1 };
	if (rwa_dispatch(c, &bind, g, s, why, sizeof(why))) { printf("%s: %s\n", argv[2], why); return 1; }
	const unsigned *o = rwa_buffer_map(b);
	int bad = 0;
	for (int i = 4; i < argc && i - 4 < 64; i++) {
		unsigned want;
		if (strchr(argv[i], '.')) { float fv = strtof(argv[i], NULL); memcpy(&want, &fv, 4); }
		else want = (unsigned)strtoul(argv[i], NULL, 10);
		if (o[i - 4] != want) {
			float got; memcpy(&got, &o[i - 4], 4);
			printf("%s: word %d is %u (%g), want %s\n", argv[2], i - 4, o[i - 4], got, argv[i]);
			bad++;
		}
	}
	printf("%s: %s\n", argv[2], bad ? "WRONG" : "ok");
	return bad != 0;
}
