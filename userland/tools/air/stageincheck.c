/*
 * stageincheck: tools/air/stage_in.ll dispatched through libhakios-air (tools/air/stageincheck.sh). Sixteen
 * threads read their [[stage_in]] float3 and write x + 10y + 100z: once with the layout a sweep compiles
 * with (buffer 30, packed), and with any argument once more with a stage-in registered as a driver would
 * (rwa_stage_in_register: buffer 5, a stride of 16, at byte 4).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n); fread(bc, 1, n, f); fclose(f);
	char why[512] = ""; struct rwa_device *d = rwa_device_open(why, sizeof(why));
	int reg = argc > 2;
	if (reg) {
		static struct rwa_stage_in si; si.attribute[0].format = 30; si.attribute[0].offset = 4; si.attribute[0].buffer_index = 5;
		si.layout[5].stride = 16; si.layout[5].step_function = 5; si.layout[5].step_rate = 1;
		rwa_stage_in_register(bc, n, &si);
	}
	struct rwa_compute_info info; struct rwa_compute *c = rwa_compute_create(d, bc, n, "si", &info, why, sizeof(why));
	if (!c) { printf("%s\n", why); return 1; }
	struct rwa_buffer *out = rwa_buffer_create(d, 64, why, sizeof(why)), *pos = rwa_buffer_create(d, 16 * 16, why, sizeof(why));
	float *p = rwa_buffer_map(pos);
	unsigned st = reg ? 4 : 3, off = reg ? 1 : 0;
	for (int i = 0; i < 16; i++) { p[st*i+off] = i; p[st*i+off+1] = i + 1; p[st*i+off+2] = 2; }
	struct rwa_buffer *b[31] = { 0 }; b[0] = out; b[reg ? 5 : 30] = pos;
	struct rwa_bindings bind = { .buffers = b, .buffer_count = 31 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { 16, 1, 1 };
	if (rwa_dispatch(c, &bind, g, s, why, sizeof(why))) { printf("%s\n", why); return 1; }
	float *o = rwa_buffer_map(out); int bad = 0;
	for (int i = 0; i < 16; i++) { float e = i + (i + 1) * 10 + 200; if (o[i] != e) { bad++; printf("%d: %g want %g\n", i, o[i], e); } }
	printf("%s\n", bad ? "WRONG" : "stage_in float3 per thread: ok");
	return bad;
}
